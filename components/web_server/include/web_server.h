#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 服务。
 *
 * 会注册本地配置页和全部 REST API。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t web_server_init(void);

/**
 * @brief 停止 Web 服务。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t web_server_stop(void);

/**
 * @brief 注入 PT1000 相关能力。
 *
 * @param[in] read_temp 温度读取函数。
 * @param[in] calibrate 校准函数。
 */
void web_server_set_pt1000_funcs(float (*read_temp)(void), esp_err_t (*calibrate)(int32_t));

/**
 * @brief 注入 OTA 启动能力。
 *
 * @param[in] ota_start OTA 启动函数。
 */
void web_server_set_ota_func(void (*ota_start)(const char *url));

#ifdef __cplusplus
}
#endif
