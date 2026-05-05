#include "web_server.h"
#include "web_html.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "wifi_mgr.h"
#include "net_transparent.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WEB_SRV";
static httpd_handle_t s_server = NULL;

/* 通过函数指针接入外部能力，避免 Web 层直接依赖具体业务实现。 */
static float (*s_pt1000_read_temp)(void) = NULL;
static esp_err_t (*s_pt1000_calibrate)(int32_t ref_ohm) = NULL;
static void (*s_ota_start)(const char *url) = NULL;

static bool json_get_u32(cJSON *root, const char *key, uint32_t *out)
{
    /* 兼容数字和字符串数字两种输入形式，减少前端表单处理负担。 */
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !out) {
        return false;
    }

    if (cJSON_IsNumber(item)) {
        *out = (uint32_t)item->valuedouble;
        return true;
    }

    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char *end = NULL;
        unsigned long val = strtoul(item->valuestring, &end, 10);
        if (end && *end == '\0') {
            *out = (uint32_t)val;
            return true;
        }
    }

    return false;
}

static bool json_get_u16(cJSON *root, const char *key, uint16_t *out)
{
    uint32_t value = 0;
    if (!json_get_u32(root, key, &value) || value > 0xFFFFu) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool json_get_u8(cJSON *root, const char *key, uint8_t *out)
{
    uint32_t value = 0;
    if (!json_get_u32(root, key, &value) || value > 0xFFu) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

void web_server_set_pt1000_funcs(float (*read_temp)(void), esp_err_t (*calibrate)(int32_t))
{
    s_pt1000_read_temp = read_temp;
    s_pt1000_calibrate = calibrate;
}

void web_server_set_ota_func(void (*ota_start)(const char *url))
{
    s_ota_start = ota_start;
}

static esp_err_t set_cors_headers(httpd_req_t *req)
{
    /* 允许浏览器和调试工具跨域访问，便于本地调试与维护。 */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    /* 根路径直接返回内嵌式配置页面。 */
    httpd_resp_set_type(req, "text/html");
    set_cors_headers(req);
    return httpd_resp_send(req, HTML_INDEX, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    /* 将当前运行配置序列化为 JSON，供页面初始化表单。 */
    dtu_config_t cfg;
    dtu_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", cfg.wifi_pass);
    cJSON_AddNumberToObject(root, "wifi_mode", cfg.wifi_mode);
    cJSON_AddStringToObject(root, "srv1_ip", cfg.srv1_ip);
    cJSON_AddNumberToObject(root, "srv1_port", cfg.srv1_port);
    cJSON_AddStringToObject(root, "srv2_ip", cfg.srv2_ip);
    cJSON_AddNumberToObject(root, "srv2_port", cfg.srv2_port);
    cJSON_AddNumberToObject(root, "uart1_mode", cfg.uart1_mode);
    cJSON_AddNumberToObject(root, "uart2_mode", cfg.uart2_mode);
    cJSON_AddNumberToObject(root, "uart1_baud", cfg.uart1_baud);
    cJSON_AddNumberToObject(root, "uart1_parity", cfg.uart1_parity);
    cJSON_AddNumberToObject(root, "uart1_stop", cfg.uart1_stop);
    cJSON_AddNumberToObject(root, "uart2_baud", cfg.uart2_baud);
    cJSON_AddNumberToObject(root, "uart2_parity", cfg.uart2_parity);
    cJSON_AddNumberToObject(root, "uart2_stop", cfg.uart2_stop);
    cJSON_AddStringToObject(root, "mqtt_broker", cfg.mqtt_broker);
    cJSON_AddStringToObject(root, "mqtt_client_id", cfg.mqtt_client_id);
    cJSON_AddStringToObject(root, "mqtt_user", cfg.mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_pass", cfg.mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_pub_topic", cfg.mqtt_pub_topic);
    cJSON_AddStringToObject(root, "mqtt_sub_topic", cfg.mqtt_sub_topic);
    cJSON_AddStringToObject(root, "mqtt_pub_topic1", cfg.mqtt_pub_topic1);
    cJSON_AddStringToObject(root, "mqtt_sub_topic1", cfg.mqtt_sub_topic1);
    cJSON_AddStringToObject(root, "mqtt_pub_topic2", cfg.mqtt_pub_topic2);
    cJSON_AddStringToObject(root, "mqtt_sub_topic2", cfg.mqtt_sub_topic2);
    cJSON_AddNumberToObject(root, "mqtt_use_tls", cfg.mqtt_use_tls);
    cJSON_AddStringToObject(root, "http_url", cfg.http_url);
    cJSON_AddStringToObject(root, "http_url1", cfg.http_url1);
    cJSON_AddStringToObject(root, "http_url2", cfg.http_url2);
    cJSON_AddStringToObject(root, "ota_url", cfg.ota_url);
    cJSON_AddNumberToObject(root, "uart_bind", cfg.uart_bind);
    cJSON_AddNumberToObject(root, "heartbeat_en", cfg.heartbeat_en);
    cJSON_AddNumberToObject(root, "heartbeat_interval", cfg.heartbeat_interval);
    cJSON_AddNumberToObject(root, "reconnect_interval", cfg.reconnect_interval);

    char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    cJSON_free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_put_handler(httpd_req_t *req)
{
    /* 允许按字段局部更新，未出现的字段保持原值不变。 */
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return httpd_resp_send_500(req);
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *item;
    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (item && cJSON_IsString(item)) dtu_config_set_str("wifi_ssid", item->valuestring);
    item = cJSON_GetObjectItem(root, "wifi_pass");
    if (item && cJSON_IsString(item)) dtu_config_set_str("wifi_pass", item->valuestring);
    uint8_t v_u8;
    uint16_t v_u16;
    uint32_t v_u32;

    if (json_get_u8(root, "wifi_mode", &v_u8)) dtu_config_set_u8("wifi_mode", v_u8);
    item = cJSON_GetObjectItem(root, "srv1_ip");
    if (item && cJSON_IsString(item)) dtu_config_set_str("srv1_ip", item->valuestring);
    if (json_get_u16(root, "srv1_port", &v_u16)) dtu_config_set_u16("srv1_port", v_u16);
    item = cJSON_GetObjectItem(root, "srv2_ip");
    if (item && cJSON_IsString(item)) dtu_config_set_str("srv2_ip", item->valuestring);
    if (json_get_u16(root, "srv2_port", &v_u16)) dtu_config_set_u16("srv2_port", v_u16);
    if (json_get_u8(root, "uart1_mode", &v_u8)) dtu_config_set_u8("uart1_mode", v_u8);
    if (json_get_u8(root, "uart2_mode", &v_u8)) dtu_config_set_u8("uart2_mode", v_u8);
    if (json_get_u32(root, "uart1_baud", &v_u32)) dtu_config_set_u32("uart1_baud", v_u32);
    if (json_get_u8(root, "uart1_parity", &v_u8)) dtu_config_set_u8("uart1_parity", v_u8);
    if (json_get_u8(root, "uart1_stop", &v_u8)) dtu_config_set_u8("uart1_stop", v_u8);
    if (json_get_u32(root, "uart2_baud", &v_u32)) dtu_config_set_u32("uart2_baud", v_u32);
    if (json_get_u8(root, "uart2_parity", &v_u8)) dtu_config_set_u8("uart2_parity", v_u8);
    if (json_get_u8(root, "uart2_stop", &v_u8)) dtu_config_set_u8("uart2_stop", v_u8);
    item = cJSON_GetObjectItem(root, "mqtt_broker");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_broker", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_client_id");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_client_id", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_user");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_user", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_pass");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_pass", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_pub_topic");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_pub_topic", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_sub_topic");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_sub_topic", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_pub_topic1");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_pub_topic1", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_sub_topic1");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_sub_topic1", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_pub_topic2");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_pub_topic2", item->valuestring);
    item = cJSON_GetObjectItem(root, "mqtt_sub_topic2");
    if (item && cJSON_IsString(item)) dtu_config_set_str("mqtt_sub_topic2", item->valuestring);
    if (json_get_u8(root, "mqtt_use_tls", &v_u8)) dtu_config_set_u8("mqtt_use_tls", v_u8);
    item = cJSON_GetObjectItem(root, "http_url");
    if (item && cJSON_IsString(item)) dtu_config_set_str("http_url", item->valuestring);
    item = cJSON_GetObjectItem(root, "http_url1");
    if (item && cJSON_IsString(item)) dtu_config_set_str("http_url1", item->valuestring);
    item = cJSON_GetObjectItem(root, "http_url2");
    if (item && cJSON_IsString(item)) dtu_config_set_str("http_url2", item->valuestring);
    item = cJSON_GetObjectItem(root, "ota_url");
    if (item && cJSON_IsString(item)) dtu_config_set_str("ota_url", item->valuestring);
    if (json_get_u8(root, "uart_bind", &v_u8)) dtu_config_set_u8("uart_bind", v_u8);
    if (json_get_u8(root, "heartbeat_en", &v_u8)) dtu_config_set_u8("heartbeat_en", v_u8);
    if (json_get_u32(root, "heartbeat_interval", &v_u32)) dtu_config_set_u32("heartbeat_interval", v_u32);
    if (json_get_u32(root, "reconnect_interval", &v_u32)) dtu_config_set_u32("reconnect_interval", v_u32);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_apply_handler(httpd_req_t *req)
{
    /* 某些配置要在重启后才彻底生效，因此这里显式提供 apply/restart 动作。 */
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, "{\"status\":\"ok\",\"msg\":\"restarting\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    /* 状态接口给页面做轮询展示，不承载重逻辑。 */
    const char *wifi_status_str[] = {"disconnected", "connecting", "connected", "failed"};
    uint8_t wifi_st = wifi_mgr_get_status();
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_status", wifi_st < 4 ? wifi_status_str[wifi_st] : "unknown");
    cJSON_AddNumberToObject(root, "net_status", net_transparent_get_status());
    cJSON_AddNumberToObject(root, "free_heap", free_heap);
    cJSON_AddNumberToObject(root, "uptime", uptime);

    char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    cJSON_free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t temperature_handler(httpd_req_t *req)
{
    float temp = 0;
    if (s_pt1000_read_temp) {
        temp = s_pt1000_read_temp();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp_c", temp);

    char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    cJSON_free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t calibrate_handler(httpd_req_t *req)
{
    /* 校准接口允许前端传入参考电阻值，默认按 1000 欧处理。 */
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) buf[ret] = '\0';

    int32_t ref_ohm = 1000;
    if (ret > 0) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *item = cJSON_GetObjectItem(root, "ref_ohm");
            if (item && cJSON_IsNumber(item)) ref_ohm = item->valueint;
            cJSON_Delete(root);
        }
    }

    esp_err_t err = ESP_FAIL;
    if (s_pt1000_calibrate) {
        err = s_pt1000_calibrate(ref_ohm);
    }

    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"status\":\"error\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t blackbox_handler(httpd_req_t *req)
{
    /* 提供最近 N 条黑匣子记录，避免一次性拉取过多数据。 */
    int count = 50;
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "count", val, sizeof(val)) == ESP_OK) {
            count = atoi(val);
            if (count <= 0 || count > 100) count = 50;
        }
    }

    blackbox_entry_t *entries = malloc(count * sizeof(blackbox_entry_t));
    if (!entries) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int n = blackbox_read_recent(entries, count);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char buf[128];
        blackbox_format_entry(&entries[i], buf, sizeof(buf));
        cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
    }
    cJSON_AddItemToObject(root, "entries", arr);
    cJSON_AddNumberToObject(root, "count", n);

    char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    cJSON_free(resp);
    cJSON_Delete(root);
    free(entries);
    return ESP_OK;
}

static esp_err_t blackbox_delete_handler(httpd_req_t *req)
{
    blackbox_clear();
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t log_config_put_handler(httpd_req_t *req)
{
    /* 日志开关既支持全开/全关，也支持按模块单独控制。 */
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return httpd_resp_send_500(req);
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *en = cJSON_GetObjectItem(root, "enable_all");
        if (en && cJSON_IsBool(en)) {
            dtu_log_enable_all(cJSON_IsTrue(en));
        }
        cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
        if (enabled && cJSON_IsArray(enabled)) {
            dtu_log_enable_all(false);
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, enabled) {
                if (!cJSON_IsString(item) || !item->valuestring) {
                    continue;
                }
                for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
                    static const char *mod_names[] = {
                        "MAIN", "WIFI", "RS485", "NET", "WEB",
                        "BLE", "PT1000", "CMD", "OTA", "CONFIG", "BLACKBOX"
                    };
                    if (strcmp(item->valuestring, mod_names[i]) == 0) {
                        dtu_log_enable((dtu_log_mod_t)i, true);
                        break;
                    }
                }
            }
        }
        cJSON_Delete(root);
    }

    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t log_config_get_handler(httpd_req_t *req)
{
    static const char *mod_names[] = {
        "MAIN", "WIFI", "RS485", "NET", "WEB",
        "BLE", "PT1000", "CMD", "OTA", "CONFIG", "BLACKBOX"
    };

    cJSON *root = cJSON_CreateObject();
    cJSON *enabled = cJSON_CreateArray();
    for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
        if (dtu_log_is_enabled((dtu_log_mod_t)i)) {
            cJSON_AddItemToArray(enabled, cJSON_CreateString(mod_names[i]));
        }
    }
    cJSON_AddItemToObject(root, "enabled", enabled);

    char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    cJSON_free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return httpd_resp_send_500(req);
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (url_item && cJSON_IsString(url_item) && s_ota_start) {
        s_ota_start(url_item->valuestring);
        httpd_resp_set_type(req, "application/json");
        set_cors_headers(req);
        httpd_resp_send(req, "{\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_type(req, "application/json");
        set_cors_headers(req);
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"missing url\"}", HTTPD_RESP_USE_STRLEN);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static const httpd_uri_t uris[] = {
    /* URI 表集中描述 Web 页面和 REST API 路由。 */
    { .uri = "/",                    .method = HTTP_GET,    .handler = index_handler },
    { .uri = "/api/v1/config",       .method = HTTP_GET,    .handler = config_get_handler },
    { .uri = "/api/v1/config",       .method = HTTP_PUT,    .handler = config_put_handler },
    { .uri = "/api/v1/config/apply", .method = HTTP_POST,   .handler = config_apply_handler },
    { .uri = "/api/v1/status",       .method = HTTP_GET,    .handler = status_handler },
    { .uri = "/api/v1/status/temperature", .method = HTTP_GET, .handler = temperature_handler },
    { .uri = "/api/v1/calibrate",    .method = HTTP_POST,   .handler = calibrate_handler },
    { .uri = "/api/v1/blackbox",     .method = HTTP_GET,    .handler = blackbox_handler },
    { .uri = "/api/v1/blackbox",     .method = HTTP_DELETE, .handler = blackbox_delete_handler },
    { .uri = "/api/v1/log_config",   .method = HTTP_GET,    .handler = log_config_get_handler },
    { .uri = "/api/v1/log_config",   .method = HTTP_PUT,    .handler = log_config_put_handler },
    { .uri = "/api/v1/ota",          .method = HTTP_POST,   .handler = ota_handler },
};

esp_err_t web_server_init(void)
{
    /* HTTPD 默认参数基础上，适度增大 handler 数量和栈大小以容纳配置接口。 */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(uris) / sizeof(uris[0]) + 4;
    config.stack_size = 4096;
    config.max_open_sockets = 4;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_WEB, TAG, "web server start failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    DTU_LOGI(DTU_LOG_MOD_WEB, TAG, "web server started on port %u, max_open_sockets=%u",
             config.server_port, config.max_open_sockets);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    /* 停服接口主要给后续重启/重配场景预留。 */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
