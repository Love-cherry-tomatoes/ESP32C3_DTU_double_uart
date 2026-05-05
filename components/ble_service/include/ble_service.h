#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 服务。
 *
 * 会启动 GATT 服务并进入广播状态，供近场维护和配置使用。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t ble_service_init(void);

/**
 * @brief 主动通知当前状态。
 *
 * 当前主要作为后续扩展接口保留，用于向已连接客户端推送状态信息。
 */
void ble_service_notify_status(void);

#ifdef __cplusplus
}
#endif
