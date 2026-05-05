#include "net_transparent.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include <string.h>

static const char *TAG = "NET_HTTP";
#define HTTP_PORT_COUNT 2

/* HTTP 模式更像“按次请求”，因此上下文只保存 URL 和回调。 */
typedef struct {
    bool ready;
    char url[128];
    void (*rx_cb)(const uint8_t *data, uint16_t len);
    uint8_t port;
} net_http_ctx_t;

static net_http_ctx_t s_ctx[HTTP_PORT_COUNT] = {
    { .port = 0 },
    { .port = 1 },
};

static bool http_port_valid(uint8_t port)
{
    return port < HTTP_PORT_COUNT;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    /* 若服务端返回正文，则原样转交给上层，作为下行数据。 */
    net_http_ctx_t *ctx = (net_http_ctx_t *)evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->rx_cb && evt->data_len > 0) {
            ctx->rx_cb((const uint8_t *)evt->data, (uint16_t)evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t net_http_start(uint8_t port, const dtu_config_t *cfg)
{
    /* HTTP 启动阶段仅校验并缓存 URL，不建立长连接。 */
    if (!cfg || !http_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_http_ctx_t *ctx = &s_ctx[port];
    const char *url = (port == 0) ? cfg->http_url1 : cfg->http_url2;
    if (strlen(url) == 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "HTTP url%u is empty", (unsigned)(port + 1));
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(ctx->url, url, sizeof(ctx->url));
    ctx->ready = true;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "HTTP channel%u ready, url=%s", (unsigned)(port + 1), ctx->url);
    return ESP_OK;
}

void net_http_stop(uint8_t port)
{
    if (!http_port_valid(port)) return;
    s_ctx[port].ready = false;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "HTTP channel%u stopped", (unsigned)(port + 1));
}

esp_err_t net_http_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 每次发送都临时创建 HTTP client，适合轻量 POST 透传场景。 */
    if (!http_port_valid(port)) return ESP_ERR_INVALID_ARG;

    net_http_ctx_t *ctx = &s_ctx[port];
    if (!ctx->ready) return ESP_ERR_INVALID_STATE;

    esp_http_client_config_t config = {
        .url = ctx->url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 5000,
        .user_data = ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "HTTP client%u init failed", (unsigned)(port + 1));
        return ESP_FAIL;
    }

    esp_http_client_set_post_field(client, (const char *)data, len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "HTTP channel%u POST failed: %s",
                 (unsigned)(port + 1), esp_err_to_name(err));
    }

    return (err == ESP_OK && status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

bool net_http_is_ready(uint8_t port)
{
    return http_port_valid(port) ? s_ctx[port].ready : false;
}

void net_http_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len))
{
    if (!http_port_valid(port)) return;
    s_ctx[port].rx_cb = cb;
}
