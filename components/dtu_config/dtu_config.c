#include "dtu_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DTU_CFG";
static const char *NVS_NAMESPACE = "dtu_cfg";
/* s_config 是系统运行期的唯一配置真相源，所有读写都围绕它展开。 */
static dtu_config_t s_config;
/* 保护配置结构与 NVS 读写，避免多个任务同时改配置造成不一致。 */
static SemaphoreHandle_t s_mutex;

ESP_EVENT_DEFINE_BASE(DTU_CONFIG_EVENT);

static const dtu_config_t s_default_config = {
    /* 这里定义的是“出厂默认值”。
     * 当 NVS 首次为空或读取失败时，系统会回退到这组参数。 */
    .wifi_ssid          = "xiaowu",
    .wifi_pass          = "Wsy17820416856",
    .wifi_mode          = DTU_WIFI_MODE_STA,
    .srv_ip             = "192.168.1.100",
    .srv_port           = 8080,
    .srv1_ip            = "192.168.1.100",
    .srv1_port          = 8080,
    .srv2_ip            = "192.168.1.100",
    .srv2_port          = 8080,
    .transparent_mode   = DTU_TRANSPARENT_MODE_TCP,
    .uart1_mode         = DTU_TRANSPARENT_MODE_TCP,
    .uart2_mode         = DTU_TRANSPARENT_MODE_TCP,
    .uart1_baud         = 9600,
    .uart1_parity       = DTU_PARITY_NONE,
    .uart1_stop         = 1,
    .uart2_baud         = 9600,
    .uart2_parity       = DTU_PARITY_NONE,
    .uart2_stop         = 1,
    .mqtt_broker        = "",
    .mqtt_client_id     = "esp32c3_dtu",
    .mqtt_user          = "",
    .mqtt_pass          = "",
    .mqtt_pub_topic     = "dtu/up",
    .mqtt_sub_topic     = "dtu/down",
    .mqtt_pub_topic1    = "dtu/ch1/up",
    .mqtt_sub_topic1    = "dtu/ch1/down",
    .mqtt_pub_topic2    = "dtu/ch2/up",
    .mqtt_sub_topic2    = "dtu/ch2/down",
    .mqtt_use_tls       = 0,
    .http_url           = "",
    .http_url1          = "",
    .http_url2          = "",
    .ota_url            = "",
    .uart_bind          = DTU_UART_BIND_BOTH,
    .heartbeat_en       = 1,
    .heartbeat_interval = 30,
    .reconnect_interval = 10,
};

static esp_err_t nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t max_len)
{
    /* 读取失败直接把错误交给上层处理；上层会保留默认值，不强制报错退出。 */
    size_t len = max_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t nvs_write_str(nvs_handle_t h, const char *key, const char *value)
{
    return nvs_set_str(h, key, value);
}

static esp_err_t load_config_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* 第一次上电或命名空间不存在时，直接使用默认配置启动。 */
        memcpy(&s_config, &s_default_config, sizeof(dtu_config_t));
        return ESP_OK;
    }

    /* 先整体填入默认值，再按键覆盖已持久化字段。
     * 这样即便后续新增了字段，老版本 NVS 中没有该键时也能自动得到合理默认值。 */
    memcpy(&s_config, &s_default_config, sizeof(dtu_config_t));

    nvs_read_str(handle, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    nvs_read_str(handle, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    nvs_get_u8(handle, "wifi_mode", &s_config.wifi_mode);
    nvs_read_str(handle, "srv_ip", s_config.srv_ip, sizeof(s_config.srv_ip));
    nvs_get_u16(handle, "srv_port", &s_config.srv_port);
    nvs_read_str(handle, "srv1_ip", s_config.srv1_ip, sizeof(s_config.srv1_ip));
    nvs_get_u16(handle, "srv1_port", &s_config.srv1_port);
    nvs_read_str(handle, "srv2_ip", s_config.srv2_ip, sizeof(s_config.srv2_ip));
    nvs_get_u16(handle, "srv2_port", &s_config.srv2_port);
    nvs_get_u8(handle, "transparent_mode", &s_config.transparent_mode);
    nvs_get_u8(handle, "uart1_mode", &s_config.uart1_mode);
    nvs_get_u8(handle, "uart2_mode", &s_config.uart2_mode);
    nvs_get_u32(handle, "uart1_baud", &s_config.uart1_baud);
    nvs_get_u8(handle, "uart1_parity", &s_config.uart1_parity);
    nvs_get_u8(handle, "uart1_stop", &s_config.uart1_stop);
    nvs_get_u32(handle, "uart2_baud", &s_config.uart2_baud);
    nvs_get_u8(handle, "uart2_parity", &s_config.uart2_parity);
    nvs_get_u8(handle, "uart2_stop", &s_config.uart2_stop);
    nvs_read_str(handle, "mqtt_broker", s_config.mqtt_broker, sizeof(s_config.mqtt_broker));
    nvs_read_str(handle, "mqtt_client_id", s_config.mqtt_client_id, sizeof(s_config.mqtt_client_id));
    nvs_read_str(handle, "mqtt_user", s_config.mqtt_user, sizeof(s_config.mqtt_user));
    nvs_read_str(handle, "mqtt_pass", s_config.mqtt_pass, sizeof(s_config.mqtt_pass));
    nvs_read_str(handle, "mqtt_pub_topic", s_config.mqtt_pub_topic, sizeof(s_config.mqtt_pub_topic));
    nvs_read_str(handle, "mqtt_sub_topic", s_config.mqtt_sub_topic, sizeof(s_config.mqtt_sub_topic));
    nvs_read_str(handle, "mqtt_pub_topic1", s_config.mqtt_pub_topic1, sizeof(s_config.mqtt_pub_topic1));
    nvs_read_str(handle, "mqtt_sub_topic1", s_config.mqtt_sub_topic1, sizeof(s_config.mqtt_sub_topic1));
    nvs_read_str(handle, "mqtt_pub_topic2", s_config.mqtt_pub_topic2, sizeof(s_config.mqtt_pub_topic2));
    nvs_read_str(handle, "mqtt_sub_topic2", s_config.mqtt_sub_topic2, sizeof(s_config.mqtt_sub_topic2));
    nvs_get_u8(handle, "mqtt_use_tls", &s_config.mqtt_use_tls);
    nvs_read_str(handle, "http_url", s_config.http_url, sizeof(s_config.http_url));
    nvs_read_str(handle, "http_url1", s_config.http_url1, sizeof(s_config.http_url1));
    nvs_read_str(handle, "http_url2", s_config.http_url2, sizeof(s_config.http_url2));
    nvs_read_str(handle, "ota_url", s_config.ota_url, sizeof(s_config.ota_url));
    nvs_get_u8(handle, "uart_bind", &s_config.uart_bind);
    nvs_get_u8(handle, "heartbeat_en", &s_config.heartbeat_en);
    nvs_get_u32(handle, "heartbeat_interval", &s_config.heartbeat_interval);
    nvs_get_u32(handle, "reconnect_interval", &s_config.reconnect_interval);

    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_config_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_write_str(handle, "wifi_ssid", s_config.wifi_ssid);
    nvs_write_str(handle, "wifi_pass", s_config.wifi_pass);
    nvs_set_u8(handle, "wifi_mode", s_config.wifi_mode);
    nvs_write_str(handle, "srv_ip", s_config.srv_ip);
    nvs_set_u16(handle, "srv_port", s_config.srv_port);
    nvs_write_str(handle, "srv1_ip", s_config.srv1_ip);
    nvs_set_u16(handle, "srv1_port", s_config.srv1_port);
    nvs_write_str(handle, "srv2_ip", s_config.srv2_ip);
    nvs_set_u16(handle, "srv2_port", s_config.srv2_port);
    nvs_set_u8(handle, "transparent_mode", s_config.transparent_mode);
    nvs_set_u8(handle, "uart1_mode", s_config.uart1_mode);
    nvs_set_u8(handle, "uart2_mode", s_config.uart2_mode);
    nvs_set_u32(handle, "uart1_baud", s_config.uart1_baud);
    nvs_set_u8(handle, "uart1_parity", s_config.uart1_parity);
    nvs_set_u8(handle, "uart1_stop", s_config.uart1_stop);
    nvs_set_u32(handle, "uart2_baud", s_config.uart2_baud);
    nvs_set_u8(handle, "uart2_parity", s_config.uart2_parity);
    nvs_set_u8(handle, "uart2_stop", s_config.uart2_stop);
    nvs_write_str(handle, "mqtt_broker", s_config.mqtt_broker);
    nvs_write_str(handle, "mqtt_client_id", s_config.mqtt_client_id);
    nvs_write_str(handle, "mqtt_user", s_config.mqtt_user);
    nvs_write_str(handle, "mqtt_pass", s_config.mqtt_pass);
    nvs_write_str(handle, "mqtt_pub_topic", s_config.mqtt_pub_topic);
    nvs_write_str(handle, "mqtt_sub_topic", s_config.mqtt_sub_topic);
    nvs_write_str(handle, "mqtt_pub_topic1", s_config.mqtt_pub_topic1);
    nvs_write_str(handle, "mqtt_sub_topic1", s_config.mqtt_sub_topic1);
    nvs_write_str(handle, "mqtt_pub_topic2", s_config.mqtt_pub_topic2);
    nvs_write_str(handle, "mqtt_sub_topic2", s_config.mqtt_sub_topic2);
    nvs_set_u8(handle, "mqtt_use_tls", s_config.mqtt_use_tls);
    nvs_write_str(handle, "http_url", s_config.http_url);
    nvs_write_str(handle, "http_url1", s_config.http_url1);
    nvs_write_str(handle, "http_url2", s_config.http_url2);
    nvs_write_str(handle, "ota_url", s_config.ota_url);
    nvs_set_u8(handle, "uart_bind", s_config.uart_bind);
    nvs_set_u8(handle, "heartbeat_en", s_config.heartbeat_en);
    nvs_set_u32(handle, "heartbeat_interval", s_config.heartbeat_interval);
    nvs_set_u32(handle, "reconnect_interval", s_config.reconnect_interval);

    /* 所有字段写入完成后统一提交，保证一次配置更新尽量原子化落盘。 */
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t dtu_config_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    /* 启动阶段只做一次配置装载，运行期由各 set 接口负责内存与 NVS 同步。 */
    esp_err_t err = load_config_from_nvs();
    if (err != ESP_OK) {
        memcpy(&s_config, &s_default_config, sizeof(dtu_config_t));
    }

    ESP_LOGI(TAG, "config loaded: ssid=%s uart1_mode=%u uart2_mode=%u socket1=%s:%u socket2=%s:%u",
             s_config.wifi_ssid,
             s_config.uart1_mode, s_config.uart2_mode,
             s_config.srv1_ip, s_config.srv1_port,
             s_config.srv2_ip, s_config.srv2_port);
    return ESP_OK;
}

esp_err_t dtu_config_get(dtu_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* 返回副本而不是内部指针，避免外部绕过锁直接修改全局配置。 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(cfg, &s_config, sizeof(dtu_config_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t dtu_config_set(const dtu_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* 完整覆盖适合“整页保存”场景，例如 Web 表单一次性提交所有配置。 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_config, cfg, sizeof(dtu_config_t));
    save_config_to_nvs();
    xSemaphoreGive(s_mutex);

    /* 通过事件通知其他模块“配置已变更”，是否立即生效由订阅方自行决定。 */
    esp_event_post(DTU_CONFIG_EVENT, DTU_CONFIG_CHANGED, NULL, 0, pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t dtu_config_set_str(const char *key, const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 这里采用显式字符串匹配而非反射式映射，优点是简单直观、易于控制每个字段边界。 */
    if (strcmp(key, "wifi_ssid") == 0) strlcpy(s_config.wifi_ssid, value, sizeof(s_config.wifi_ssid));
    else if (strcmp(key, "wifi_pass") == 0) strlcpy(s_config.wifi_pass, value, sizeof(s_config.wifi_pass));
    else if (strcmp(key, "srv_ip") == 0) strlcpy(s_config.srv_ip, value, sizeof(s_config.srv_ip));
    else if (strcmp(key, "srv1_ip") == 0) strlcpy(s_config.srv1_ip, value, sizeof(s_config.srv1_ip));
    else if (strcmp(key, "srv2_ip") == 0) strlcpy(s_config.srv2_ip, value, sizeof(s_config.srv2_ip));
    else if (strcmp(key, "mqtt_broker") == 0) strlcpy(s_config.mqtt_broker, value, sizeof(s_config.mqtt_broker));
    else if (strcmp(key, "mqtt_client_id") == 0) strlcpy(s_config.mqtt_client_id, value, sizeof(s_config.mqtt_client_id));
    else if (strcmp(key, "mqtt_user") == 0) strlcpy(s_config.mqtt_user, value, sizeof(s_config.mqtt_user));
    else if (strcmp(key, "mqtt_pass") == 0) strlcpy(s_config.mqtt_pass, value, sizeof(s_config.mqtt_pass));
    else if (strcmp(key, "mqtt_pub_topic") == 0) strlcpy(s_config.mqtt_pub_topic, value, sizeof(s_config.mqtt_pub_topic));
    else if (strcmp(key, "mqtt_sub_topic") == 0) strlcpy(s_config.mqtt_sub_topic, value, sizeof(s_config.mqtt_sub_topic));
    else if (strcmp(key, "mqtt_pub_topic1") == 0) strlcpy(s_config.mqtt_pub_topic1, value, sizeof(s_config.mqtt_pub_topic1));
    else if (strcmp(key, "mqtt_sub_topic1") == 0) strlcpy(s_config.mqtt_sub_topic1, value, sizeof(s_config.mqtt_sub_topic1));
    else if (strcmp(key, "mqtt_pub_topic2") == 0) strlcpy(s_config.mqtt_pub_topic2, value, sizeof(s_config.mqtt_pub_topic2));
    else if (strcmp(key, "mqtt_sub_topic2") == 0) strlcpy(s_config.mqtt_sub_topic2, value, sizeof(s_config.mqtt_sub_topic2));
    else if (strcmp(key, "http_url") == 0) strlcpy(s_config.http_url, value, sizeof(s_config.http_url));
    else if (strcmp(key, "http_url1") == 0) strlcpy(s_config.http_url1, value, sizeof(s_config.http_url1));
    else if (strcmp(key, "http_url2") == 0) strlcpy(s_config.http_url2, value, sizeof(s_config.http_url2));
    else if (strcmp(key, "ota_url") == 0) strlcpy(s_config.ota_url, value, sizeof(s_config.ota_url));
    else {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    save_config_to_nvs();
    xSemaphoreGive(s_mutex);
    esp_event_post(DTU_CONFIG_EVENT, DTU_CONFIG_CHANGED, NULL, 0, pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t dtu_config_set_u8(const char *key, uint8_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* U8 类字段主要是模式枚举与布尔开关。 */
    if (strcmp(key, "wifi_mode") == 0) s_config.wifi_mode = value;
    else if (strcmp(key, "transparent_mode") == 0) s_config.transparent_mode = value;
    else if (strcmp(key, "uart1_mode") == 0) s_config.uart1_mode = value;
    else if (strcmp(key, "uart2_mode") == 0) s_config.uart2_mode = value;
    else if (strcmp(key, "uart1_parity") == 0) s_config.uart1_parity = value;
    else if (strcmp(key, "uart1_stop") == 0) s_config.uart1_stop = value;
    else if (strcmp(key, "uart2_parity") == 0) s_config.uart2_parity = value;
    else if (strcmp(key, "uart2_stop") == 0) s_config.uart2_stop = value;
    else if (strcmp(key, "mqtt_use_tls") == 0) s_config.mqtt_use_tls = value;
    else if (strcmp(key, "uart_bind") == 0) s_config.uart_bind = value;
    else if (strcmp(key, "heartbeat_en") == 0) s_config.heartbeat_en = value;
    else {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    save_config_to_nvs();
    xSemaphoreGive(s_mutex);
    esp_event_post(DTU_CONFIG_EVENT, DTU_CONFIG_CHANGED, NULL, 0, pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t dtu_config_set_u16(const char *key, uint16_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 当前 U16 主要用于端口号。 */
    if (strcmp(key, "srv_port") == 0) s_config.srv_port = value;
    else if (strcmp(key, "srv1_port") == 0) s_config.srv1_port = value;
    else if (strcmp(key, "srv2_port") == 0) s_config.srv2_port = value;
    else {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    save_config_to_nvs();
    xSemaphoreGive(s_mutex);
    esp_event_post(DTU_CONFIG_EVENT, DTU_CONFIG_CHANGED, NULL, 0, pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t dtu_config_set_u32(const char *key, uint32_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 当前 U32 主要覆盖波特率、心跳间隔、重连间隔等参数。 */
    if (strcmp(key, "uart1_baud") == 0) s_config.uart1_baud = value;
    else if (strcmp(key, "uart2_baud") == 0) s_config.uart2_baud = value;
    else if (strcmp(key, "heartbeat_interval") == 0) s_config.heartbeat_interval = value;
    else if (strcmp(key, "reconnect_interval") == 0) s_config.reconnect_interval = value;
    else {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    save_config_to_nvs();
    xSemaphoreGive(s_mutex);
    esp_event_post(DTU_CONFIG_EVENT, DTU_CONFIG_CHANGED, NULL, 0, pdMS_TO_TICKS(100));
    return ESP_OK;
}
