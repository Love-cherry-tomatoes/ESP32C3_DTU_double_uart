#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief WiFi 已断开。 */
#define WIFI_MGR_STATUS_DISCONNECTED  0
/** @brief WiFi 正在连接。 */
#define WIFI_MGR_STATUS_CONNECTING    1
/** @brief WiFi 已连接。 */
#define WIFI_MGR_STATUS_CONNECTED     2
/** @brief WiFi 连接失败。 */
#define WIFI_MGR_STATUS_FAILED        3

/**
 * @brief 初始化 WiFi 管理器。
 *
 * 会根据 `dtu_config` 中的设置启动 STA、AP 或 APSTA。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief 以 STA 模式连接指定热点。
 *
 * @param[in] ssid 目标热点名称。
 * @param[in] pass 热点密码，可为 `NULL`。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass);

/**
 * @brief 启动 AP 模式。
 *
 * 通常用于现场配置，或 STA 多次连接失败后的回退入口。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t wifi_mgr_start_ap(void);

/**
 * @brief 获取当前 WiFi 状态。
 *
 * @return 状态值，取值见 `WIFI_MGR_STATUS_*`。
 */
uint8_t wifi_mgr_get_status(void);

/**
 * @brief 获取最近一次断开原因的可读字符串。
 *
 * @return 指向静态字符串的指针。
 */
const char *wifi_mgr_get_disconnect_reason(void);

/**
 * @brief 获取最近一次断开原因的原始错误码。
 *
 * @return 原始 WiFi 断开原因码。
 */
int wifi_mgr_get_disconnect_reason_code(void);

#ifdef __cplusplus
}
#endif
