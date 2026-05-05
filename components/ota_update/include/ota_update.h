#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 OTA 模块。
 *
 * 会检查当前镜像状态，并在需要时确认镜像有效。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t ota_update_init(void);

/**
 * @brief 异步启动 OTA 升级。
 *
 * @param[in] url 固件下载地址。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t ota_update_start(const char *url);

/**
 * @brief 获取 OTA 进度。
 *
 * @return 当前 OTA 进度值。
 */
int ota_update_get_progress(void);

/**
 * @brief 查询 OTA 是否正在进行。
 *
 * @return `true` 表示进行中，`false` 表示未进行。
 */
bool ota_update_is_in_progress(void);

#ifdef __cplusplus
}
#endif
