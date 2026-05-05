#include "net_transparent.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "NET_UDP";
#define UDP_PORT_COUNT 2

/* UDP 无连接，因此上下文主要保存目标地址和接收任务状态。 */
typedef struct {
    int sock;
    bool ready;
    TaskHandle_t rx_task;
    void (*rx_cb)(const uint8_t *data, uint16_t len);
    struct sockaddr_in dest_addr;
    uint8_t port;
} net_udp_ctx_t;

static net_udp_ctx_t s_ctx[UDP_PORT_COUNT] = {
    { .sock = -1, .port = 0 },
    { .sock = -1, .port = 1 },
};

static bool udp_port_valid(uint8_t port)
{
    return port < UDP_PORT_COUNT;
}

static void udp_rx_task(void *arg)
{
    /* UDP 接收任务持续监听 socket，收到数据后原样交给上层回调。 */
    net_udp_ctx_t *ctx = (net_udp_ctx_t *)arg;
    uint8_t buf[512];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    while (ctx->ready) {
        int len = recvfrom(ctx->sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src_addr, &addr_len);
        if (len <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                DTU_LOGW(DTU_LOG_MOD_NET, TAG, "port%u recvfrom failed, errno=%d",
                         (unsigned)(ctx->port + 1), errno);
            }
            continue;
        }
        if (ctx->rx_cb) {
            ctx->rx_cb(buf, (uint16_t)len);
        }
    }

    ctx->rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t net_udp_start(uint8_t port, const dtu_config_t *cfg)
{
    /* UDP 启动本质上是“建 socket + 保存对端地址 + 起接收任务”。 */
    if (!cfg || !udp_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_udp_ctx_t *ctx = &s_ctx[port];
    const char *ip = (port == 0) ? cfg->srv1_ip : cfg->srv2_ip;
    uint16_t target_port = (port == 0) ? cfg->srv1_port : cfg->srv2_port;

    if (ip[0] == '\0' || target_port == 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "UDP port%u target not configured", (unsigned)(port + 1));
        return ESP_ERR_INVALID_ARG;
    }

    ctx->dest_addr.sin_family = AF_INET;
    ctx->dest_addr.sin_port = htons(target_port);
    inet_aton(ip, &ctx->dest_addr.sin_addr);

    ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (ctx->sock < 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "UDP socket%u create failed", (unsigned)(port + 1));
        BB_ERROR(BB_CAT_NET, BB_EVT_UDP_SOCKET_FAIL, errno, "sock_err");
        return ESP_FAIL;
    }

    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(ctx->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ctx->ready = true;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "UDP socket%u ready, target=%s:%u",
             (unsigned)(port + 1), ip, target_port);
    BB_INFO(BB_CAT_NET, BB_EVT_UDP_SOCKET_OK, port, "ok");

    if (xTaskCreate(udp_rx_task, port == 0 ? "udp0_rx" : "udp1_rx", 3072, ctx, 5, &ctx->rx_task) != pdTRUE) {
        ctx->ready = false;
        close(ctx->sock);
        ctx->sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_udp_stop(uint8_t port)
{
    /* 关闭 socket 后，接收任务会在超时或错误返回后自行退出。 */
    if (!udp_port_valid(port)) return;

    net_udp_ctx_t *ctx = &s_ctx[port];
    ctx->ready = false;
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    if (ctx->rx_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "UDP socket%u stopped", (unsigned)(port + 1));
}

esp_err_t net_udp_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    if (!udp_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_udp_ctx_t *ctx = &s_ctx[port];
    if (!ctx->ready || ctx->sock < 0) return ESP_ERR_INVALID_STATE;

    int ret = sendto(ctx->sock, data, len, 0,
                     (struct sockaddr *)&ctx->dest_addr, sizeof(ctx->dest_addr));
    if (ret < 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "UDP socket%u sendto failed, errno=%d",
                 (unsigned)(port + 1), errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool net_udp_is_ready(uint8_t port)
{
    return udp_port_valid(port) ? s_ctx[port].ready : false;
}

void net_udp_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len))
{
    if (!udp_port_valid(port)) return;
    s_ctx[port].rx_cb = cb;
}
