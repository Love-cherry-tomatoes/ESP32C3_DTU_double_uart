#include "net_transparent.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/tcp.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NET_TCP";
#define TCP_PORT_COUNT 2

/* 每一路 TCP 通道维护自己的 socket、回调和接收任务。 */
typedef struct {
    int sock;
    bool connected;
    TaskHandle_t rx_task;
    void (*rx_cb)(const uint8_t *data, uint16_t len);
    SemaphoreHandle_t mutex;
    char srv_ip[46];
    uint16_t srv_port;
    uint8_t port;
} net_tcp_ctx_t;

static net_tcp_ctx_t s_ctx[TCP_PORT_COUNT] = {
    { .sock = -1, .port = 0 },
    { .sock = -1, .port = 1 },
};

static bool tcp_port_valid(uint8_t port)
{
    return port < TCP_PORT_COUNT;
}

static void tcp_rx_task(void *arg)
{
    /* 独立接收任务负责阻塞式收包，并在断线时做本地清理。 */
    net_tcp_ctx_t *ctx = (net_tcp_ctx_t *)arg;
    uint8_t buf[512];
    while (ctx->connected) {
        int len = recv(ctx->sock, buf, sizeof(buf), 0);
        if (len <= 0) {
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            DTU_LOGW(DTU_LOG_MOD_NET, TAG, "socket%u recv failed, len=%d errno=%d",
                     (unsigned)(ctx->port + 1), len, errno);
            BB_ERROR(BB_CAT_NET, BB_EVT_TCP_DISCONNECTED, len, "recv_err");
            break;
        }
        if (ctx->rx_cb) {
            ctx->rx_cb(buf, (uint16_t)len);
        }
    }

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->connected = false;
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    xSemaphoreGive(ctx->mutex);
    ctx->rx_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t tcp_connect_channel(net_tcp_ctx_t *ctx)
{
    /* 连接逻辑被抽成内部函数，便于首次启动和后续重连复用。 */
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(ctx->srv_port);
    inet_aton(ctx->srv_ip, &dest_addr.sin_addr);

    ctx->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ctx->sock < 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "socket%u create failed", (unsigned)(ctx->port + 1));
        BB_ERROR(BB_CAT_NET, BB_EVT_TCP_CONNECT_FAIL, errno, "sock_err");
        return ESP_FAIL;
    }

    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "socket%u connecting to %s:%u...",
             (unsigned)(ctx->port + 1), ctx->srv_ip, ctx->srv_port);

    int ret = connect(ctx->sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (ret != 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "socket%u connect failed, errno=%d",
                 (unsigned)(ctx->port + 1), errno);
        BB_ERROR(BB_CAT_NET, BB_EVT_TCP_CONNECT_FAIL, errno, "conn_err");
        close(ctx->sock);
        ctx->sock = -1;
        return ESP_FAIL;
    }

    /* 收发超时和 keepalive 参数有助于更快发现死连接。 */
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(ctx->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(ctx->sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int keepalive = 1;
    int keepidle = 30;
    int keepintvl = 10;
    int keepcnt = 3;
    setsockopt(ctx->sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(ctx->sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(ctx->sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(ctx->sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    ctx->connected = true;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "socket%u connected to %s:%u",
             (unsigned)(ctx->port + 1), ctx->srv_ip, ctx->srv_port);
    BB_INFO(BB_CAT_NET, BB_EVT_TCP_CONNECT_OK, ctx->port, "ok");

    xTaskCreate(tcp_rx_task, ctx->port == 0 ? "tcp0_rx" : "tcp1_rx", 3072, ctx, 5, &ctx->rx_task);
    return ESP_OK;
}

esp_err_t net_tcp_start(uint8_t port, const dtu_config_t *cfg)
{
    /* 双路 TCP 共用一套实现，通过 port 参数区分目标服务器配置。 */
    if (!cfg || !tcp_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_tcp_ctx_t *ctx = &s_ctx[port];
    const char *srv_ip = "";
    uint16_t srv_port = 0;

    if (port == 0) {
        srv_ip = cfg->srv1_ip;
        srv_port = cfg->srv1_port;
    } else {
        srv_ip = cfg->srv2_ip;
        srv_port = cfg->srv2_port;
    }

    if (srv_ip[0] == '\0' || srv_port == 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "socket%u config invalid: ip=\"%s\" port=%u",
                 (unsigned)(port + 1), srv_ip, (unsigned)srv_port);
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->mutex) {
        ctx->mutex = xSemaphoreCreateMutex();
        if (!ctx->mutex) return ESP_ERR_NO_MEM;
    }

    strlcpy(ctx->srv_ip, srv_ip, sizeof(ctx->srv_ip));
    ctx->srv_port = srv_port;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t err = tcp_connect_channel(ctx);
    xSemaphoreGive(ctx->mutex);
    return err;
}

void net_tcp_stop(uint8_t port)
{
    /* 停止时先标记 disconnected，再关闭 socket，让接收任务尽快退出。 */
    if (!tcp_port_valid(port)) {
        return;
    }

    net_tcp_ctx_t *ctx = &s_ctx[port];
    if (!ctx->mutex) {
        return;
    }

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->connected = false;
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    xSemaphoreGive(ctx->mutex);

    if (ctx->rx_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "socket%u stopped", (unsigned)(port + 1));
}

esp_err_t net_tcp_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 发送前仅检查当前连接状态，不做自动重连，由上层重连任务统一处理。 */
    if (!tcp_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_tcp_ctx_t *ctx = &s_ctx[port];
    if (!ctx->connected || ctx->sock < 0) return ESP_ERR_INVALID_STATE;

    int written = send(ctx->sock, data, len, 0);
    if (written < 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "socket%u send failed, errno=%d",
                 (unsigned)(port + 1), errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool net_tcp_is_connected(uint8_t port)
{
    if (!tcp_port_valid(port)) return false;
    return s_ctx[port].connected;
}

void net_tcp_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len))
{
    if (!tcp_port_valid(port)) {
        return;
    }
    s_ctx[port].rx_cb = cb;
}
