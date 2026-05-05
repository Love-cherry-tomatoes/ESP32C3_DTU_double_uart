#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 命令来源：TCP 通道 0。 */
#define CMD_SRC_TCP0  0
/** @brief 命令来源：TCP，兼容 `CMD_SRC_TCP0`。 */
#define CMD_SRC_TCP   CMD_SRC_TCP0
/** @brief 命令来源：UDP。 */
#define CMD_SRC_UDP   1
/** @brief 命令来源：MQTT。 */
#define CMD_SRC_MQTT  2
/** @brief 命令来源：BLE。 */
#define CMD_SRC_BLE   3
/** @brief 命令来源：HTTP。 */
#define CMD_SRC_HTTP  4
/** @brief 命令来源：TCP 通道 1。 */
#define CMD_SRC_TCP1  5

/** @brief 命令帧前缀。 */
#define CMD_PREFIX       "+++DTU_CMD:"
/** @brief 命令帧前缀长度。 */
#define CMD_PREFIX_LEN   10
/** @brief 命令帧后缀。 */
#define CMD_SUFFIX       "+++"
/** @brief 命令帧后缀长度。 */
#define CMD_SUFFIX_LEN   3

/**
 * @brief 命令响应回调类型。
 *
 * @param[in] source 原始命令来源。
 * @param[in] response 响应字符串。
 * @param[in] len 响应长度。
 */
typedef void (*cmd_response_cb_t)(uint8_t source, const char *response, uint16_t len);

/**
 * @brief 初始化命令处理模块。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t cmd_handler_init(void);

/**
 * @brief 判断一帧数据是否为 DTU 命令帧。
 *
 * @param[in] data 输入数据。
 * @param[in] len 数据长度。
 * @return `true` 表示是命令帧，`false` 表示不是。
 */
bool cmd_handler_is_command(const uint8_t *data, uint16_t len);

/**
 * @brief 异步提交一条命令请求。
 *
 * @param[in] source 命令来源。
 * @param[in] data 命令帧数据。
 * @param[in] len 数据长度。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t cmd_handler_submit(uint8_t source, const uint8_t *data, uint16_t len);

/**
 * @brief 注册命令响应回调。
 *
 * @param[in] cb 回调函数。
 */
void cmd_handler_set_response_cb(cmd_response_cb_t cb);

/**
 * @brief 注入 PT1000 相关业务能力。
 *
 * @param[in] read_temp 温度读取函数。
 * @param[in] calibrate 校准函数。
 */
void cmd_handler_set_pt1000_funcs(float (*read_temp)(void), esp_err_t (*calibrate)(int32_t));

/**
 * @brief 注入 OTA 启动能力。
 *
 * @param[in] ota_start OTA 启动函数。
 */
void cmd_handler_set_ota_func(void (*ota_start)(const char *url));

#ifdef __cplusplus
}
#endif
