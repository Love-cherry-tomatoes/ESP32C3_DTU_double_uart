#include "rs485_uart.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include <string.h>

#define TAG "rs485_uart"

#define RX_BUF_SIZE     256
#define TX_BUF_SIZE     0       /* manual TX direction control */
#define RX_QUEUE_LEN    8
#define RX_TASK_STACK   3072
#define RX_TASK_PRIO    5
#define TX_DONE_TIMEOUT_MS  100

/* 
 * 每一路 RS485 端口的运行态描述。
 * 这里除了保存硬件引脚，还保存接收队列和接收任务句柄，
 * 说明本模块同时承担“硬件驱动 + 数据解耦”的职责。
 */
typedef struct {
    uart_port_t  uart_num;
    gpio_num_t   tx_pin;
    gpio_num_t   rx_pin;
    gpio_num_t   de_re_pin;
    QueueHandle_t rx_queue;
    TaskHandle_t  rx_task;
    bool          initialized;
} rs485_port_t;

static rs485_port_t s_ports[2] = {
    [0] = {   /* RS485-A */
        .uart_num  = RS485_A_UART_NUM,
        .tx_pin    = RS485_A_TX_PIN,
        .rx_pin    = RS485_A_RX_PIN,
        .de_re_pin = RS485_A_DE_RE_PIN,
        .rx_queue  = NULL,
        .rx_task   = NULL,
        .initialized = false,
    },
    [1] = {   /* RS485-B */
        .uart_num  = RS485_B_UART_NUM,
        .tx_pin    = RS485_B_TX_PIN,
        .rx_pin    = RS485_B_RX_PIN,
        .de_re_pin = RS485_B_DE_RE_PIN,
        .rx_queue  = NULL,
        .rx_task   = NULL,
        .initialized = false,
    },
};

/* ---- 基础辅助函数 ---- */

static uart_parity_t parity_to_uart(uint8_t parity)
{
    switch (parity) {
    case DTU_PARITY_EVEN: return UART_PARITY_EVEN;
    case DTU_PARITY_ODD:  return UART_PARITY_ODD;
    default:              return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t stop_bits_to_uart(uint8_t stop_bits)
{
    return (stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static bool port_index_valid(uint8_t port)
{
    return (port == 0 || port == 1);
}

static bool pin_is_usb_serial_jtag(gpio_num_t pin)
{
    return (pin == GPIO_NUM_18 || pin == GPIO_NUM_19);
}

static bool rs485_port_conflicts_with_console(const rs485_port_t *p)
{
    /* 某些引脚可能与 USB Serial/JTAG 控制台复用。
     * 若冲突则主动跳过初始化，避免下载/日志串口与业务串口互相干扰。 */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return pin_is_usb_serial_jtag(p->tx_pin) ||
           pin_is_usb_serial_jtag(p->rx_pin) ||
           pin_is_usb_serial_jtag(p->de_re_pin);
#else
    (void)p;
    return false;
#endif
}

/* ---- DE/RE 方向控制 ---- */

static void de_re_enable_tx(rs485_port_t *p)
{
    /* 半双工 RS485 发送前拉高 DE/RE，切到发送方向。 */
    gpio_set_level(p->de_re_pin, 1);
}

static void de_re_enable_rx(rs485_port_t *p)
{
    /* 发送完成后切回接收方向，避免错过总线返回数据。 */
    gpio_set_level(p->de_re_pin, 0);
}

/* ---- 单路串口初始化 ---- */

static esp_err_t rs485_configure_port(rs485_port_t *p, uint32_t baud,
                                       uart_parity_t parity,
                                       uart_stop_bits_t stop_bits)
{
    uart_config_t uart_config = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = parity,
        .stop_bits  = stop_bits,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_param_config(p->uart_num, &uart_config);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(p->uart_num, p->tx_pin, p->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t rs485_install_driver(rs485_port_t *p)
{
    /* 只申请 RX 缓冲，不启用 UART 驱动内部 TX 缓冲。
     * 发送方向由本模块手动控制，更符合 RS485 半双工场景。 */
    esp_err_t err = uart_driver_install(p->uart_num, RX_BUF_SIZE, TX_BUF_SIZE,
                                         0, NULL, 0);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t rs485_init_de_re_gpio(rs485_port_t *p)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << p->de_re_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "gpio_config DE/RE failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 上电默认进入接收模式，避免设备一启动就长期占用总线。 */
    de_re_enable_rx(p);
    return ESP_OK;
}

/* ---- 接收任务 ---- */

static void rs485_rx_task(void *arg)
{
    rs485_port_t *p = (rs485_port_t *)arg;
    /* 为每个接收任务独立分配一块临时缓冲，再封装成固定结构体压入队列。 */
    uint8_t *buf = malloc(RS485_MAX_DATA_LEN);
    if (buf == NULL) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "RX task malloc failed for port %d", p->uart_num);
        vTaskDelete(NULL);
        return;
    }

    DTU_LOGI(DTU_LOG_MOD_RS485, TAG, "RX task started for UART%d", p->uart_num);

    while (1) {
        int len = uart_read_bytes(p->uart_num, buf, RS485_MAX_DATA_LEN,
                                   pdMS_TO_TICKS(100));
        if (len > 0) {
            rs485_data_t rx_data;
            memset(&rx_data, 0, sizeof(rx_data));
            /* 通过端口索引标识数据来源，便于上层统一处理双路串口。 */
            rx_data.port = (uint8_t)(p - s_ports);
            rx_data.len  = (uint16_t)len;
            memcpy(rx_data.data, buf, len);

            if (xQueueSend(p->rx_queue, &rx_data, pdMS_TO_TICKS(10)) != pdTRUE) {
                DTU_LOGW(DTU_LOG_MOD_RS485, TAG, "RX queue full on UART%d, dropping %d bytes",
                         p->uart_num, len);
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/* ---- 对外接口 ---- */

esp_err_t rs485_uart_init(void)
{
    dtu_config_t cfg;
    esp_err_t err = dtu_config_get(&cfg);
    int initialized_ports = 0;
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "dtu_config_get failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 配置模块中的 uart1/uart2 分别映射到物理 RS485-A / RS485-B。 */
    uint32_t baud_a     = cfg.uart1_baud;
    uint8_t  parity_a   = cfg.uart1_parity;
    uint8_t  stop_a     = cfg.uart1_stop;

    /* RS485-B: uart2 settings */
    uint32_t baud_b     = cfg.uart2_baud;
    uint8_t  parity_b   = cfg.uart2_parity;
    uint8_t  stop_b     = cfg.uart2_stop;

    for (int i = 0; i < 2; i++) {
        rs485_port_t *p = &s_ports[i];
        uint32_t baud;
        uart_parity_t parity;
        uart_stop_bits_t stop;

        if (i == 0) {
            baud   = baud_a;
            parity = parity_to_uart(parity_a);
            stop   = stop_bits_to_uart(stop_a);
        } else {
            baud   = baud_b;
            parity = parity_to_uart(parity_b);
            stop   = stop_bits_to_uart(stop_b);
        }

        if (rs485_port_conflicts_with_console(p)) {
            DTU_LOGW(DTU_LOG_MOD_RS485, TAG,
                     "Skipping RS485-%c init: pin conflict with USB Serial JTAG console "
                     "(tx=%d rx=%d de_re=%d)",
                     i == 0 ? 'A' : 'B', p->tx_pin, p->rx_pin, p->de_re_pin);
            BB_WARN(BB_CAT_UART, 0, i, "usb_jtag_conflict");
            continue;
        }

        err = rs485_configure_port(p, baud, parity, stop);
        if (err != ESP_OK) {
            BB_ERROR(BB_CAT_UART, 0, err, "cfg fail");
            return err;
        }

        err = rs485_install_driver(p);
        if (err != ESP_OK) {
            BB_ERROR(BB_CAT_UART, 0, err, "drv fail");
            return err;
        }

        err = rs485_init_de_re_gpio(p);
        if (err != ESP_OK) {
            BB_ERROR(BB_CAT_UART, 0, err, "gpio fail");
            return err;
        }

        /* 为每路串口建立独立接收队列，让上层按端口消费数据。 */
        p->rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rs485_data_t));
        if (p->rx_queue == NULL) {
            DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "xQueueCreate failed for port %d", i);
            BB_ERROR(BB_CAT_UART, 0, 0, "q fail");
            return ESP_ERR_NO_MEM;
        }

        /* 每一路串口对应一个接收任务，简化双路并发读串口的处理。 */
        char task_name[16];
        snprintf(task_name, sizeof(task_name), "rs485_rx_%d", i);
        BaseType_t ret = xTaskCreate(rs485_rx_task, task_name,
                                      RX_TASK_STACK, p, RX_TASK_PRIO,
                                      &p->rx_task);
        if (ret != pdTRUE) {
            DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "xTaskCreate failed for port %d", i);
            BB_ERROR(BB_CAT_UART, 0, 0, "task fail");
            return ESP_ERR_NO_MEM;
        }

        p->initialized = true;
        initialized_ports++;
        DTU_LOGI(DTU_LOG_MOD_RS485, TAG, "RS485-%c initialized: UART%d %lu %d%c%d",
                 i == 0 ? 'A' : 'B', p->uart_num, baud,
                 8, parity == UART_PARITY_EVEN ? 'E' : (parity == UART_PARITY_ODD ? 'O' : 'N'),
                 stop == UART_STOP_BITS_2 ? 2 : 1);
    }

    if (initialized_ports == 0) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "No RS485 ports initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* 只要至少有一路可用，系统仍可继续运行。 */
    BB_INFO(BB_CAT_UART, 0, 0, "init ok");
    return ESP_OK;
}

esp_err_t rs485_uart_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    if (!port_index_valid(port)) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "send: invalid port %d", port);
        return ESP_ERR_INVALID_ARG;
    }

    rs485_port_t *p = &s_ports[port];
    if (!p->initialized) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "send: port %d not initialized", port);
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 半双工发送时，必须先切换收发方向。 */
    de_re_enable_tx(p);

    /* 直接写硬件 FIFO，由于没有启用 TX 缓冲，返回值更贴近底层发送状态。 */
    int written = uart_write_bytes(p->uart_num, data, len);
    if (written < 0) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "uart_write_bytes failed on UART%d", p->uart_num);
        de_re_enable_rx(p);
        BB_ERROR(BB_CAT_UART, 0, written, "tx fail");
        return ESP_FAIL;
    }

    /* 等待最后一个字节真正发完，再切回接收方向。 */
    esp_err_t err = uart_wait_tx_done(p->uart_num, pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS));
    if (err != ESP_OK) {
        DTU_LOGW(DTU_LOG_MOD_RS485, TAG, "uart_wait_tx_done timeout on UART%d", p->uart_num);
    }

    /* 无论是否超时，都尽快恢复接收模式。 */
    de_re_enable_rx(p);

    return ESP_OK;
}

QueueHandle_t rs485_uart_get_rx_queue(uint8_t port)
{
    if (!port_index_valid(port)) {
        return NULL;
    }
    return s_ports[port].rx_queue;
}

esp_err_t rs485_uart_set_baud(uint8_t port, uint32_t baud)
{
    if (!port_index_valid(port)) {
        return ESP_ERR_INVALID_ARG;
    }

    rs485_port_t *p = &s_ports[port];
    if (!p->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uart_set_baudrate(p->uart_num, baud);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "set_baud %lu failed on UART%d: %s",
                 baud, p->uart_num, esp_err_to_name(err));
        return err;
    }

    DTU_LOGI(DTU_LOG_MOD_RS485, TAG, "UART%d baud changed to %lu", p->uart_num, baud);
    return ESP_OK;
}

esp_err_t rs485_uart_set_config(uint8_t port, uint32_t baud, uint8_t parity, uint8_t stop_bits)
{
    if (!port_index_valid(port)) {
        return ESP_ERR_INVALID_ARG;
    }

    rs485_port_t *p = &s_ports[port];
    if (!p->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_parity_t     uart_parity = parity_to_uart(parity);
    uart_stop_bits_t  uart_stop   = stop_bits_to_uart(stop_bits);

    /* ESP-IDF 的部分 UART 参数修改更稳妥的方式是删驱动后重建。 */
    uart_driver_delete(p->uart_num);

    esp_err_t err = rs485_configure_port(p, baud, uart_parity, uart_stop);
    if (err != ESP_OK) {
        /* 如果重配置失败，至少尝试把驱动重新装回去，避免端口彻底失效。 */
        rs485_install_driver(p);
        BB_ERROR(BB_CAT_UART, 0, err, "reconf fail");
        return err;
    }

    err = rs485_install_driver(p);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_RS485, TAG, "reinstall driver failed on UART%d", p->uart_num);
        p->initialized = false;
        BB_ERROR(BB_CAT_UART, 0, err, "reinst fail");
        return err;
    }

    DTU_LOGI(DTU_LOG_MOD_RS485, TAG, "UART%d reconfigured: %lu %d%c%d",
             p->uart_num, baud, 8,
             uart_parity == UART_PARITY_EVEN ? 'E' : (uart_parity == UART_PARITY_ODD ? 'O' : 'N'),
             uart_stop == UART_STOP_BITS_2 ? 2 : 1);
    BB_INFO(BB_CAT_UART, 0, baud, "reconf ok");

    return ESP_OK;
}
