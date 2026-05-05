#pragma once

#include "esp_log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 日志模块枚举。
 */
typedef enum {
    DTU_LOG_MOD_MAIN = 0,
    DTU_LOG_MOD_WIFI,
    DTU_LOG_MOD_RS485,
    DTU_LOG_MOD_NET,
    DTU_LOG_MOD_WEB,
    DTU_LOG_MOD_BLE,
    DTU_LOG_MOD_PT1000,
    DTU_LOG_MOD_CMD,
    DTU_LOG_MOD_OTA,
    DTU_LOG_MOD_CONFIG,
    DTU_LOG_MOD_BLACKBOX,
    DTU_LOG_MOD_MAX
} dtu_log_mod_t;

/**
 * @brief 初始化日志模块开关状态。
 *
 * 若 NVS 中存在历史配置，则按历史配置恢复；否则默认全部开启。
 */
void dtu_log_init(void);

/**
 * @brief 设置单个模块日志开关。
 *
 * @param[in] mod 目标模块。
 * @param[in] enable `true` 为开启，`false` 为关闭。
 */
void dtu_log_enable(dtu_log_mod_t mod, bool enable);

/**
 * @brief 查询模块日志是否已开启。
 *
 * @param[in] mod 目标模块。
 * @return `true` 表示开启，`false` 表示关闭。
 */
bool dtu_log_is_enabled(dtu_log_mod_t mod);

/**
 * @brief 一次性设置全部模块日志开关。
 *
 * @param[in] enable `true` 为全部开启，`false` 为全部关闭。
 */
void dtu_log_enable_all(bool enable);

/** @brief 信息级日志输出宏，附带函数名和行号。 */
#define DTU_LOGI(mod, tag, format, ...) \
    do { \
        if (dtu_log_is_enabled(mod)) { \
            ESP_LOGI(tag, "[%s:%d] " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } \
    } while(0)

/** @brief 警告级日志输出宏，附带函数名和行号。 */
#define DTU_LOGW(mod, tag, format, ...) \
    do { \
        if (dtu_log_is_enabled(mod)) { \
            ESP_LOGW(tag, "[%s:%d] " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } \
    } while(0)

/** @brief 错误级日志输出宏，附带函数名和行号。 */
#define DTU_LOGE(mod, tag, format, ...) \
    do { \
        if (dtu_log_is_enabled(mod)) { \
            ESP_LOGE(tag, "[%s:%d] " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } \
    } while(0)

/** @brief 调试级日志输出宏，附带函数名和行号。 */
#define DTU_LOGD(mod, tag, format, ...) \
    do { \
        if (dtu_log_is_enabled(mod)) { \
            ESP_LOGD(tag, "[%s:%d] " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif
