#include "net_transparent.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "cmd_handler.h"
#include "rs485_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include <string.h>

static const char *TAG = "NET_TRANS";
/* 透明传输层是串口与网络协议之间的总调度器。 */
#define UART_PORT_COUNT 2
static const uint8_t UART_QUEUE_LEN = 8;
static const uint8_t NET_TO_UART_QUEUE_LEN = 8;
static const uint16_t NET_TX_TASK_STACK = 3072;
static const uint16_t NET_UART_TX_TASK_STACK = 2048;
static const uint16_t NET_RECONNECT_TASK_STACK = 2048;

static uint8_t s_modes[UART_PORT_COUNT] = {
    NET_TRANS_MODE_TCP,
    NET_TRANS_MODE_TCP,
};
static bool s_running = false;
static SemaphoreHandle_t s_mutex;

static QueueHandle_t s_uart_a_to_net_queue = NULL;
static QueueHandle_t s_uart_b_to_net_queue = NULL;
static QueueHandle_t s_net_to_uart_a_queue = NULL;
static QueueHandle_t s_net_to_uart_b_queue = NULL;

static TaskHandle_t s_net_tx_task_handle = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;

static dtu_config_t s_cfg;

static void log_net_rx(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 仅打印可读字符预览，避免日志被二进制数据污染。 */
    char preview[49];
    uint16_t n = len < (sizeof(preview) - 1) ? len : (sizeof(preview) - 1);

    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        preview[i] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    preview[n] = '\0';

    switch (s_modes[port]) {
    case NET_TRANS_MODE_TCP:
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "RX TCP socket%u len=%u preview=\"%s\"",
                 (unsigned)(port + 1), (unsigned)len, preview);
        break;
    case NET_TRANS_MODE_UDP:
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "RX UDP len=%u preview=\"%s\"",
                 (unsigned)len, preview);
        break;
    case NET_TRANS_MODE_MQTT:
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "RX MQTT len=%u preview=\"%s\"",
                 (unsigned)len, preview);
        break;
    case NET_TRANS_MODE_HTTP:
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "RX HTTP len=%u preview=\"%s\"",
                 (unsigned)len, preview);
        break;
    default:
        break;
    }
}

extern esp_err_t net_tcp_start(uint8_t port, const dtu_config_t *cfg);
extern void net_tcp_stop(uint8_t port);
extern esp_err_t net_tcp_send(uint8_t port, const uint8_t *data, uint16_t len);
extern bool net_tcp_is_connected(uint8_t port);
extern void net_tcp_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len));

extern esp_err_t net_udp_start(uint8_t port, const dtu_config_t *cfg);
extern void net_udp_stop(uint8_t port);
extern esp_err_t net_udp_send(uint8_t port, const uint8_t *data, uint16_t len);
extern bool net_udp_is_ready(uint8_t port);
extern void net_udp_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len));

extern esp_err_t net_mqtt_start(const dtu_config_t *cfg);
extern void net_mqtt_stop(void);
extern esp_err_t net_mqtt_send(uint8_t port, const uint8_t *data, uint16_t len);
extern bool net_mqtt_is_connected(void);
extern void net_mqtt_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len));

extern esp_err_t net_http_start(uint8_t port, const dtu_config_t *cfg);
extern void net_http_stop(uint8_t port);
extern esp_err_t net_http_send(uint8_t port, const uint8_t *data, uint16_t len);
extern bool net_http_is_ready(uint8_t port);
extern void net_http_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len));

static uint8_t current_cmd_source(uint8_t port)
{
    /* 命令处理器需要知道数据来自哪个通道，才能把响应路由回去。 */
    switch (s_modes[port]) {
    case NET_TRANS_MODE_TCP:
        return (port == 0) ? CMD_SRC_TCP0 : CMD_SRC_TCP1;
    case NET_TRANS_MODE_UDP:
        return CMD_SRC_UDP;
    case NET_TRANS_MODE_MQTT:
        return CMD_SRC_MQTT;
    case NET_TRANS_MODE_HTTP:
        return CMD_SRC_HTTP;
    default:
        return CMD_SRC_TCP;
    }
}

static void on_net_rx_data_for_port(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 下行数据若识别为命令帧，则不再转发到串口，而是直接交给命令处理器。 */
    log_net_rx(port, data, len);

    if (cmd_handler_is_command(data, len)) {
        cmd_handler_submit(current_cmd_source(port), data, len);
        return;
    }

    net_data_t item = {0};
    item.port = port;
    item.len = (len > NET_DATA_MAX_LEN) ? NET_DATA_MAX_LEN : len;
    memcpy(item.data, data, item.len);
    QueueHandle_t q = (port == 0) ? s_net_to_uart_a_queue : s_net_to_uart_b_queue;
    if (xQueueSend(q, &item, pdMS_TO_TICKS(20)) != pdTRUE) {
        DTU_LOGW(DTU_LOG_MOD_NET, TAG, "socket%u -> uart queue full, dropping %u bytes",
                 (unsigned)(port + 1), (unsigned)item.len);
    }
}

static void on_net_rx_data_port0(const uint8_t *data, uint16_t len)
{
    on_net_rx_data_for_port(0, data, len);
}

static void on_net_rx_data_port1(const uint8_t *data, uint16_t len)
{
    on_net_rx_data_for_port(1, data, len);
}

static void net_tx_task(void *arg)
{
    (void)arg;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "net_tx_task started, modes=%u/%u", s_modes[0], s_modes[1]);
    net_data_t item;

    while (1) {
        /* 轮询双路串口上行队列，把数据发往各自当前绑定的网络协议。 */
        if (xQueueReceive(s_uart_a_to_net_queue, &item, pdMS_TO_TICKS(50))) {
        } else if (xQueueReceive(s_uart_b_to_net_queue, &item, pdMS_TO_TICKS(50))) {
        } else {
            continue;
        }

        if (cmd_handler_is_command(item.data, item.len)) {
            cmd_handler_submit(current_cmd_source(item.port), item.data, item.len);
            continue;
        }

        esp_err_t err = ESP_FAIL;
        switch (s_modes[item.port]) {
        case NET_TRANS_MODE_TCP:
            err = net_tcp_send(item.port, item.data, item.len);
            break;
        case NET_TRANS_MODE_UDP:
            err = net_udp_send(item.port, item.data, item.len);
            break;
        case NET_TRANS_MODE_MQTT:
            err = net_mqtt_send(item.port, item.data, item.len);
            break;
        case NET_TRANS_MODE_HTTP:
            err = net_http_send(item.port, item.data, item.len);
            break;
        }

        if (err != ESP_OK) {
            DTU_LOGW(DTU_LOG_MOD_NET, TAG, "send failed, port=%u mode=%u err=%s",
                     (unsigned)item.port, (unsigned)s_modes[item.port], esp_err_to_name(err));
        }
    }
}

static void net_uart_tx_task(void *arg)
{
    /* 每个串口各有一个下行发送任务，专门负责把网络数据回灌到 RS485。 */
    net_data_t item;
    QueueHandle_t q = (QueueHandle_t)arg;

    while (1) {
        if (xQueueReceive(q, &item, portMAX_DELAY)) {
            rs485_uart_send(item.port, item.data, item.len);
        }
    }
}

static void net_reconnect_task(void *arg)
{
    (void)arg;
    /* 后台巡检任务，按配置周期检查并重建掉线通道。 */
    while (1) {
        dtu_config_get(&s_cfg);

        bool mqtt_needed = false;
        for (uint8_t port = 0; port < UART_PORT_COUNT; port++) {
            switch (s_modes[port]) {
            case NET_TRANS_MODE_TCP:
                if (!net_tcp_is_connected(port)) {
                    DTU_LOGW(DTU_LOG_MOD_NET, TAG, "TCP socket%u disconnected, reconnecting...",
                             (unsigned)(port + 1));
                    net_tcp_stop(port);
                    esp_err_t err = net_tcp_start(port, &s_cfg);
                    if (err != ESP_OK) {
                        DTU_LOGW(DTU_LOG_MOD_NET, TAG, "TCP socket%u reconnect failed: %s",
                                 (unsigned)(port + 1), esp_err_to_name(err));
                    }
                }
                break;
            case NET_TRANS_MODE_MQTT:
                mqtt_needed = true;
                break;
            default:
                break;
            }
        }

        if (mqtt_needed && !net_mqtt_is_connected()) {
            DTU_LOGW(DTU_LOG_MOD_NET, TAG, "MQTT disconnected, reconnecting...");
            net_mqtt_stop();
            net_mqtt_start(&s_cfg);
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.reconnect_interval * 1000U));
    }
}

static void stop_channel(uint8_t port)
{
    /* 根据当前模式停止对应协议通道。 */
    switch (s_modes[port]) {
    case NET_TRANS_MODE_TCP:
        net_tcp_stop(port);
        break;
    case NET_TRANS_MODE_UDP:
        net_udp_stop(port);
        break;
    case NET_TRANS_MODE_HTTP:
        net_http_stop(port);
        break;
    default:
        break;
    }
}

static esp_err_t start_channel(uint8_t port)
{
    /* 启动通道前先绑定对应的接收回调。 */
    void (*rx_cb)(const uint8_t *data, uint16_t len) = (port == 0) ? on_net_rx_data_port0 : on_net_rx_data_port1;

    switch (s_modes[port]) {
    case NET_TRANS_MODE_TCP:
        net_tcp_set_rx_callback(port, rx_cb);
        return net_tcp_start(port, &s_cfg);
    case NET_TRANS_MODE_UDP:
        net_udp_set_rx_callback(port, rx_cb);
        return net_udp_start(port, &s_cfg);
    case NET_TRANS_MODE_MQTT:
        net_mqtt_set_rx_callback(port, rx_cb);
        return ESP_OK;
    case NET_TRANS_MODE_HTTP:
        net_http_set_rx_callback(port, rx_cb);
        return net_http_start(port, &s_cfg);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t net_transparent_init(void)
{
    /* 分别为双路串口上行和双路网络下行建立独立队列。 */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_uart_a_to_net_queue = xQueueCreate(UART_QUEUE_LEN, sizeof(net_data_t));
    s_uart_b_to_net_queue = xQueueCreate(UART_QUEUE_LEN, sizeof(net_data_t));
    s_net_to_uart_a_queue = xQueueCreate(NET_TO_UART_QUEUE_LEN, sizeof(net_data_t));
    s_net_to_uart_b_queue = xQueueCreate(NET_TO_UART_QUEUE_LEN, sizeof(net_data_t));

    if (!s_uart_a_to_net_queue || !s_uart_b_to_net_queue ||
        !s_net_to_uart_a_queue || !s_net_to_uart_b_queue) {
        return ESP_ERR_NO_MEM;
    }

    dtu_config_get(&s_cfg);
    s_modes[0] = s_cfg.uart1_mode;
    s_modes[1] = s_cfg.uart2_mode;

    esp_err_t err = ESP_OK;
    /* MQTT 是全局单客户端实现，因此只要任一路需要就统一启动。 */
    bool mqtt_needed = false;
    for (uint8_t port = 0; port < UART_PORT_COUNT; port++) {
        if (s_modes[port] == NET_TRANS_MODE_MQTT) {
            mqtt_needed = true;
        } else {
            err = start_channel(port);
            if (err != ESP_OK) {
                DTU_LOGE(DTU_LOG_MOD_NET, TAG, "start port %u mode %u failed: %s",
                         (unsigned)port, (unsigned)s_modes[port], esp_err_to_name(err));
            }
        }
    }
    if (mqtt_needed) {
        err = net_mqtt_start(&s_cfg);
        if (err != ESP_OK) {
            DTU_LOGE(DTU_LOG_MOD_NET, TAG, "start mqtt failed: %s", esp_err_to_name(err));
        }
    }

    if (xTaskCreate(net_tx_task, "net_tx", NET_TX_TASK_STACK, NULL, 6, &s_net_tx_task_handle) != pdTRUE ||
        xTaskCreate(net_uart_tx_task, "uart_a_tx", NET_UART_TX_TASK_STACK, s_net_to_uart_a_queue, 6, NULL) != pdTRUE ||
        xTaskCreate(net_uart_tx_task, "uart_b_tx", NET_UART_TX_TASK_STACK, s_net_to_uart_b_queue, 6, NULL) != pdTRUE ||
        xTaskCreate(net_reconnect_task, "net_reconn", NET_RECONNECT_TASK_STACK, NULL, 5, &s_reconnect_task_handle) != pdTRUE) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "net_transparent init done, modes=%u/%u", s_modes[0], s_modes[1]);
    return ESP_OK;
}

esp_err_t net_transparent_set_mode(uint8_t port, uint8_t mode)
{
    if (port >= UART_PORT_COUNT) return ESP_ERR_INVALID_ARG;
    if (mode > NET_TRANS_MODE_HTTP) return ESP_ERR_INVALID_ARG;

    dtu_config_get(&s_cfg);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 切模式时要先判断是否涉及 MQTT 的全局客户端重建。 */
    bool had_mqtt = false;
    bool new_mqtt_needed = false;
    for (uint8_t i = 0; i < UART_PORT_COUNT; i++) {
        if (s_modes[i] == NET_TRANS_MODE_MQTT) {
            had_mqtt = true;
        }
    }

    stop_channel(port);
    if (had_mqtt) {
        net_mqtt_stop();
    }

    s_modes[port] = mode;
    if (port == 0) {
        s_cfg.uart1_mode = mode;
    } else {
        s_cfg.uart2_mode = mode;
    }

    for (uint8_t i = 0; i < UART_PORT_COUNT; i++) {
        if (s_modes[i] == NET_TRANS_MODE_MQTT) {
            new_mqtt_needed = true;
        }
    }

    esp_err_t err = ESP_OK;
    if (new_mqtt_needed) {
        err = net_mqtt_start(&s_cfg);
        if (err == ESP_OK && mode != NET_TRANS_MODE_MQTT) {
            err = start_channel(port);
        }
    } else {
        err = start_channel(port);
    }
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) {
        dtu_config_set(&s_cfg);
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "port %u mode switched to %d", (unsigned)port, mode);
    }
    return err;
}

esp_err_t net_transparent_reconnect(void)
{
    /* 强制按当前模式完整重连全部协议通道。 */
    dtu_config_get(&s_cfg);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint8_t port = 0; port < UART_PORT_COUNT; port++) {
        stop_channel(port);
    }
    net_mqtt_stop();

    esp_err_t err = ESP_OK;
    bool mqtt_needed = false;
    for (uint8_t port = 0; port < UART_PORT_COUNT; port++) {
        if (s_modes[port] == NET_TRANS_MODE_MQTT) {
            mqtt_needed = true;
        } else {
            err = start_channel(port);
        }
    }
    if (mqtt_needed) {
        err = net_mqtt_start(&s_cfg);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

uint8_t net_transparent_get_status(void)
{
    /* 只要任一通道就绪，就认为网络侧具备基本工作能力。 */
    for (uint8_t port = 0; port < UART_PORT_COUNT; port++) {
        switch (s_modes[port]) {
        case NET_TRANS_MODE_TCP:
            if (net_tcp_is_connected(port)) return 1;
            break;
        case NET_TRANS_MODE_UDP:
            if (net_udp_is_ready(port)) return 1;
            break;
        case NET_TRANS_MODE_MQTT:
            if (net_mqtt_is_connected()) return 1;
            break;
        case NET_TRANS_MODE_HTTP:
            if (net_http_is_ready(port)) return 1;
            break;
        default:
            break;
        }
    }
    return 0;
}

QueueHandle_t net_transparent_get_uart_rx_queue(uint8_t port)
{
    return (port == 0) ? s_uart_a_to_net_queue : s_uart_b_to_net_queue;
}

void net_transparent_send_to_network(const net_data_t *data)
{
    /* 上层把串口数据投到这里，由 net_tx_task 异步发往网络。 */
    if (!data) return;
    QueueHandle_t q = (data->port == 0) ? s_uart_a_to_net_queue : s_uart_b_to_net_queue;
    if (xQueueSend(q, data, pdMS_TO_TICKS(20)) != pdTRUE) {
        DTU_LOGW(DTU_LOG_MOD_NET, TAG, "uart%u -> net queue full, dropping %u bytes",
                 (unsigned)(data->port + 1), (unsigned)data->len);
    }
}

void net_transparent_send_to_uart(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 网络侧响应或业务数据统一排队后再发串口，避免直接跨线程操作 UART。 */
    net_data_t item = {0};
    item.port = port;
    item.len = (len > NET_DATA_MAX_LEN) ? NET_DATA_MAX_LEN : len;
    memcpy(item.data, data, item.len);
    QueueHandle_t q = (port == 0) ? s_net_to_uart_a_queue : s_net_to_uart_b_queue;
    if (xQueueSend(q, &item, pdMS_TO_TICKS(20)) != pdTRUE) {
        DTU_LOGW(DTU_LOG_MOD_NET, TAG, "uart%u response queue full, dropping %u bytes",
                 (unsigned)(port + 1), (unsigned)item.len);
    }
}
