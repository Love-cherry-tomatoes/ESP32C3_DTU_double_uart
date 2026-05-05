#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 黑匣子头部魔数。 */
#define BB_MAGIC 0xBBBB0001u

/** @brief 黑匣子分类：WiFi。 */
#define BB_CAT_WIFI      0
/** @brief 黑匣子分类：网络。 */
#define BB_CAT_NET       1
/** @brief 黑匣子分类：串口。 */
#define BB_CAT_UART      2
/** @brief 黑匣子分类：MQTT。 */
#define BB_CAT_MQTT      3
/** @brief 黑匣子分类：系统。 */
#define BB_CAT_SYSTEM    4
/** @brief 黑匣子分类：OTA。 */
#define BB_CAT_OTA       5
/** @brief 黑匣子分类：PT1000。 */
#define BB_CAT_PT1000    6
/** @brief 黑匣子分类：BLE。 */
#define BB_CAT_BLE       7

/** @brief 信息级事件。 */
#define BB_LEVEL_INFO     0
/** @brief 警告级事件。 */
#define BB_LEVEL_WARN     1
/** @brief 错误级事件。 */
#define BB_LEVEL_ERROR    2
/** @brief 严重错误级事件。 */
#define BB_LEVEL_CRITICAL 3

/** @brief WiFi：开始连接。 */
#define BB_EVT_WIFI_CONNECT_START      0
/** @brief WiFi：连接成功。 */
#define BB_EVT_WIFI_CONNECTED          1
/** @brief WiFi：连接断开。 */
#define BB_EVT_WIFI_DISCONNECTED       2
/** @brief WiFi：连接失败。 */
#define BB_EVT_WIFI_CONNECT_FAILED     3
/** @brief WiFi：认证失败。 */
#define BB_EVT_WIFI_AUTH_ERROR         4

/** @brief TCP：连接成功。 */
#define BB_EVT_TCP_CONNECT_OK          0
/** @brief TCP：连接失败。 */
#define BB_EVT_TCP_CONNECT_FAIL        1
/** @brief TCP：连接断开。 */
#define BB_EVT_TCP_DISCONNECTED        2
/** @brief UDP：socket 就绪。 */
#define BB_EVT_UDP_SOCKET_OK           3
/** @brief UDP：socket 创建失败。 */
#define BB_EVT_UDP_SOCKET_FAIL         4

/** @brief MQTT：连接成功。 */
#define BB_EVT_MQTT_CONNECT_OK         0
/** @brief MQTT：连接失败。 */
#define BB_EVT_MQTT_CONNECT_FAIL       1
/** @brief MQTT：连接断开。 */
#define BB_EVT_MQTT_DISCONNECTED       2
/** @brief MQTT：订阅失败。 */
#define BB_EVT_MQTT_SUB_FAIL           3
/** @brief MQTT：发布失败。 */
#define BB_EVT_MQTT_PUB_FAIL           4

/** @brief 系统：启动。 */
#define BB_EVT_SYS_BOOT                0
/** @brief 系统：重启。 */
#define BB_EVT_SYS_RESTART             1
/** @brief 系统：看门狗触发。 */
#define BB_EVT_SYS_WDT_TRIGGER         2
/** @brief 系统：崩溃。 */
#define BB_EVT_SYS_PANIC               3
/** @brief 系统：堆内存偏低。 */
#define BB_EVT_SYS_HEAP_LOW            4
/** @brief 系统：栈溢出。 */
#define BB_EVT_SYS_STACK_OVERFLOW      5

/**
 * @brief 黑匣子单条记录。
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint8_t  category;
    uint8_t  level;
    uint8_t  event_id;
    uint8_t  reserved;
    int32_t  value;
    char     msg[16];
} blackbox_entry_t;

/** @brief 黑匣子单条记录字节大小。 */
#define BB_ENTRY_SIZE sizeof(blackbox_entry_t)

/**
 * @brief 黑匣子环形存储头部。
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_index;
    uint32_t entry_count;
    uint32_t capacity;
} blackbox_header_t;

/**
 * @brief 初始化黑匣子分区。
 *
 * 若分区为空或头部不合法，会自动擦除并重建环形结构。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t blackbox_init(void);

/**
 * @brief 写入一条黑匣子事件记录。
 *
 * @param[in] category 事件分类。
 * @param[in] level 事件级别。
 * @param[in] event_id 事件编号。
 * @param[in] value 附加数值。
 * @param[in] msg 附加短消息，可为 `NULL`。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t blackbox_record(uint8_t category, uint8_t level, uint8_t event_id,
                           int32_t value, const char *msg);

/** @brief 记录信息级黑匣子事件。 */
#define BB_INFO(cat, evt, val, msg)    blackbox_record(cat, BB_LEVEL_INFO, evt, val, msg)
/** @brief 记录警告级黑匣子事件。 */
#define BB_WARN(cat, evt, val, msg)    blackbox_record(cat, BB_LEVEL_WARN, evt, val, msg)
/** @brief 记录错误级黑匣子事件。 */
#define BB_ERROR(cat, evt, val, msg)   blackbox_record(cat, BB_LEVEL_ERROR, evt, val, msg)
/** @brief 记录严重错误级黑匣子事件。 */
#define BB_CRITICAL(cat, evt, val, msg) blackbox_record(cat, BB_LEVEL_CRITICAL, evt, val, msg)

/**
 * @brief 读取最近的若干条记录。
 *
 * @param[out] entries 输出记录数组。
 * @param[in] max_count 最多读取的条数。
 * @return 实际读取到的条数。
 */
int blackbox_read_recent(blackbox_entry_t *entries, int max_count);

/**
 * @brief 读取当前全部有效记录。
 *
 * @param[out] entries 输出记录数组。
 * @param[in] max_count 最多读取的条数。
 * @return 实际读取到的条数。
 */
int blackbox_read_all(blackbox_entry_t *entries, int max_count);

/**
 * @brief 清空黑匣子内容并重置头部。
 */
void blackbox_clear(void);

/**
 * @brief 将一条黑匣子记录格式化为文本。
 *
 * @param[in] entry 输入记录。
 * @param[out] buf 输出缓冲区。
 * @param[in] buf_len 输出缓冲区长度。
 * @return 写入的字符数。
 */
int blackbox_format_entry(const blackbox_entry_t *entry, char *buf, int buf_len);

#ifdef __cplusplus
}
#endif
