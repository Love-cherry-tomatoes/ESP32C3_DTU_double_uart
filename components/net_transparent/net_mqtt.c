#include "net_transparent.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include <string.h>

static const char *TAG = "NET_MQTT";
#define MQTT_PORT_COUNT 2

/* 每个逻辑端口维护自己的收发 topic，但底层共享一个 MQTT 客户端。 */
typedef struct {
    void (*rx_cb)(const uint8_t *data, uint16_t len);
    char pub_topic[65];
    char sub_topic[65];
} mqtt_port_ctx_t;

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static mqtt_port_ctx_t s_ports[MQTT_PORT_COUNT];
static char s_broker[128];

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "MQTT connected");
        BB_INFO(BB_CAT_MQTT, BB_EVT_MQTT_CONNECT_OK, 0, "ok");
        /* 连接成功后，按通道配置逐个订阅下行 topic。 */
        for (uint8_t port = 0; port < MQTT_PORT_COUNT; port++) {
            if (s_ports[port].sub_topic[0] != '\0') {
                esp_mqtt_client_subscribe(s_client, s_ports[port].sub_topic, 1);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        DTU_LOGW(DTU_LOG_MOD_NET, TAG, "MQTT disconnected");
        BB_ERROR(BB_CAT_MQTT, BB_EVT_MQTT_DISCONNECTED, 0, "disconn");
        break;

    case MQTT_EVENT_DATA:
        /* 收到消息后按 topic 映射回具体逻辑端口。 */
        if (event->topic && event->topic_len > 0 && event->data_len > 0) {
            for (uint8_t port = 0; port < MQTT_PORT_COUNT; port++) {
                size_t topic_len = strlen(s_ports[port].sub_topic);
                if (topic_len == (size_t)event->topic_len &&
                    topic_len > 0 &&
                    strncmp(event->topic, s_ports[port].sub_topic, topic_len) == 0) {
                    if (s_ports[port].rx_cb) {
                        s_ports[port].rx_cb((const uint8_t *)event->data, (uint16_t)event->data_len);
                    }
                }
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "MQTT error: %s",
                 event->error_handle ? "error" : "unknown");
        BB_ERROR(BB_CAT_MQTT, BB_EVT_MQTT_CONNECT_FAIL,
                 event->error_handle ? event->error_handle->error_type : -1, "err");
        break;

    default:
        break;
    }
}

esp_err_t net_mqtt_start(const dtu_config_t *cfg)
{
    /* MQTT 为全局单实例，因此会把双通道 topic 一次性装载进去。 */
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->mqtt_broker) == 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "MQTT broker URI is empty");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_broker, cfg->mqtt_broker, sizeof(s_broker));
    strlcpy(s_ports[0].pub_topic, cfg->mqtt_pub_topic1, sizeof(s_ports[0].pub_topic));
    strlcpy(s_ports[0].sub_topic, cfg->mqtt_sub_topic1, sizeof(s_ports[0].sub_topic));
    strlcpy(s_ports[1].pub_topic, cfg->mqtt_pub_topic2, sizeof(s_ports[1].pub_topic));
    strlcpy(s_ports[1].sub_topic, cfg->mqtt_sub_topic2, sizeof(s_ports[1].sub_topic));

    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = cfg->mqtt_broker;
    mqtt_cfg.credentials.client_id = cfg->mqtt_client_id;

    if (strlen(cfg->mqtt_user) > 0) {
        mqtt_cfg.credentials.username = cfg->mqtt_user;
        mqtt_cfg.credentials.authentication.password = cfg->mqtt_pass;
    }

    /* 这里只记录 TLS 开关，证书链配置还可继续扩展。 */
    if (cfg->mqtt_use_tls) {
        DTU_LOGI(DTU_LOG_MOD_NET, TAG, "MQTTS enabled");
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "MQTT client init failed");
        BB_ERROR(BB_CAT_MQTT, BB_EVT_MQTT_CONNECT_FAIL, 0, "init_err");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "MQTT started, broker=%s", cfg->mqtt_broker);
    return ESP_OK;
}

void net_mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    DTU_LOGI(DTU_LOG_MOD_NET, TAG, "MQTT stopped");
}

esp_err_t net_mqtt_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    /* 发送时按逻辑端口选择对应上行 topic。 */
    if (port >= MQTT_PORT_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_connected || !s_client) return ESP_ERR_INVALID_STATE;
    if (s_ports[port].pub_topic[0] == '\0') return ESP_ERR_INVALID_ARG;

    int msg_id = esp_mqtt_client_publish(s_client, s_ports[port].pub_topic,
                                         (const char *)data, len, 1, 0);
    if (msg_id < 0) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "MQTT publish failed on port%u", (unsigned)(port + 1));
        BB_ERROR(BB_CAT_MQTT, BB_EVT_MQTT_PUB_FAIL, msg_id, "pub_err");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool net_mqtt_is_connected(void)
{
    return s_connected;
}

void net_mqtt_set_rx_callback(uint8_t port, void (*cb)(const uint8_t *data, uint16_t len))
{
    if (port >= MQTT_PORT_COUNT) return;
    s_ports[port].rx_cb = cb;
}
