#pragma once

#include "esp_err.h"
#include "esp_event_base.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 透明传输模式：TCP。 */
#define DTU_TRANSPARENT_MODE_TCP    0
/** @brief 透明传输模式：UDP。 */
#define DTU_TRANSPARENT_MODE_UDP    1
/** @brief 透明传输模式：MQTT。 */
#define DTU_TRANSPARENT_MODE_MQTT   2
/** @brief 透明传输模式：HTTP。 */
#define DTU_TRANSPARENT_MODE_HTTP   3

/** @brief WiFi 工作模式：仅 AP。 */
#define DTU_WIFI_MODE_AP    0
/** @brief WiFi 工作模式：仅 STA。 */
#define DTU_WIFI_MODE_STA   1
/** @brief WiFi 工作模式：AP+STA。 */
#define DTU_WIFI_MODE_APSTA 2

/** @brief 串口绑定方式：双串口同时接收网络下行。 */
#define DTU_UART_BIND_BOTH  0
/** @brief 串口绑定方式：仅 UART1。 */
#define DTU_UART_BIND_UART1 1
/** @brief 串口绑定方式：仅 UART2。 */
#define DTU_UART_BIND_UART2 2

/** @brief 无校验。 */
#define DTU_PARITY_NONE 0
/** @brief 偶校验。 */
#define DTU_PARITY_EVEN 1
/** @brief 奇校验。 */
#define DTU_PARITY_ODD  2

/**
 * @brief DTU 全局运行配置。
 *
 * 该结构体集中描述 WiFi、串口、网络透传、MQTT、HTTP、OTA 等运行参数，
 * 也是 Web、BLE、命令接口共享的统一配置模型。
 */
typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    uint8_t  wifi_mode;
    char     srv_ip[46];
    uint16_t srv_port;
    char     srv1_ip[46];
    uint16_t srv1_port;
    char     srv2_ip[46];
    uint16_t srv2_port;
    uint8_t  transparent_mode;
    uint8_t  uart1_mode;
    uint8_t  uart2_mode;
    uint32_t uart1_baud;
    uint8_t  uart1_parity;
    uint8_t  uart1_stop;
    uint32_t uart2_baud;
    uint8_t  uart2_parity;
    uint8_t  uart2_stop;
    char     mqtt_broker[128];
    char     mqtt_client_id[33];
    char     mqtt_user[33];
    char     mqtt_pass[65];
    char     mqtt_pub_topic[65];
    char     mqtt_sub_topic[65];
    char     mqtt_pub_topic1[65];
    char     mqtt_sub_topic1[65];
    char     mqtt_pub_topic2[65];
    char     mqtt_sub_topic2[65];
    uint8_t  mqtt_use_tls;
    char     http_url[128];
    char     http_url1[128];
    char     http_url2[128];
    char     ota_url[128];
    uint8_t  uart_bind;
    uint8_t  heartbeat_en;
    uint32_t heartbeat_interval;
    uint32_t reconnect_interval;
} dtu_config_t;

ESP_EVENT_DECLARE_BASE(DTU_CONFIG_EVENT);

/** @brief 配置变更事件 ID。 */
#define DTU_CONFIG_CHANGED 1

/**
 * @brief 初始化配置模块。
 *
 * 会装载默认配置、读取 NVS 中的持久化配置，并准备内部互斥锁。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t dtu_config_init(void);

/**
 * @brief 读取当前完整配置快照。
 *
 * @param[out] cfg 用于接收配置副本的输出指针。
 * @return ESP_OK 表示成功；ESP_ERR_INVALID_ARG 表示参数无效。
 */
esp_err_t dtu_config_get(dtu_config_t *cfg);

/**
 * @brief 整体覆盖当前配置。
 *
 * 成功后会同步写入 NVS，并广播 `DTU_CONFIG_CHANGED` 事件。
 *
 * @param[in] cfg 新配置对象。
 * @return ESP_OK 表示成功；否则返回错误码。
 */
esp_err_t dtu_config_set(const dtu_config_t *cfg);

/**
 * @brief 按字段名更新字符串类配置。
 *
 * 适用于 SSID、IP、MQTT topic、URL 等文本参数。
 *
 * @param[in] key 配置字段名。
 * @param[in] value 新的字符串值。
 * @return ESP_OK 表示成功；ESP_ERR_NOT_FOUND 表示字段不存在。
 */
esp_err_t dtu_config_set_str(const char *key, const char *value);

/**
 * @brief 按字段名更新 U8 类配置。
 *
 * 适用于模式枚举和布尔开关等短整型参数。
 *
 * @param[in] key 配置字段名。
 * @param[in] value 新值。
 * @return ESP_OK 表示成功；ESP_ERR_NOT_FOUND 表示字段不存在。
 */
esp_err_t dtu_config_set_u8(const char *key, uint8_t value);

/**
 * @brief 按字段名更新 U16 类配置。
 *
 * 当前主要用于端口号。
 *
 * @param[in] key 配置字段名。
 * @param[in] value 新值。
 * @return ESP_OK 表示成功；ESP_ERR_NOT_FOUND 表示字段不存在。
 */
esp_err_t dtu_config_set_u16(const char *key, uint16_t value);

/**
 * @brief 按字段名更新 U32 类配置。
 *
 * 当前主要用于波特率、心跳周期、重连周期等参数。
 *
 * @param[in] key 配置字段名。
 * @param[in] value 新值。
 * @return ESP_OK 表示成功；ESP_ERR_NOT_FOUND 表示字段不存在。
 */
esp_err_t dtu_config_set_u32(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif
