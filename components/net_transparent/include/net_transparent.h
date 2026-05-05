#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 透传模式：TCP。 */
#define NET_TRANS_MODE_TCP    0
/** @brief 透传模式：UDP。 */
#define NET_TRANS_MODE_UDP    1
/** @brief 透传模式：MQTT。 */
#define NET_TRANS_MODE_MQTT   2
/** @brief 透传模式：HTTP。 */
#define NET_TRANS_MODE_HTTP   3

/** @brief 透传数据最大长度。 */
#define NET_DATA_MAX_LEN 256

/**
 * @brief 网络透传数据封装。
 */
typedef struct {
    uint8_t  port;
    uint8_t  data[NET_DATA_MAX_LEN];
    uint16_t len;
} net_data_t;

/**
 * @brief 初始化透明传输引擎。
 *
 * 会创建串口上行队列、网络下行队列以及相关任务，并按配置启动对应通道。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t net_transparent_init(void);

/**
 * @brief 切换指定通道的透传模式。
 *
 * @param[in] port 目标通道，`0` 表示通道 1，`1` 表示通道 2。
 * @param[in] mode 新模式，取值见 `NET_TRANS_MODE_*`。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t net_transparent_set_mode(uint8_t port, uint8_t mode);

/**
 * @brief 按当前配置重建全部网络通道。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t net_transparent_reconnect(void);

/**
 * @brief 获取整体网络可用状态。
 *
 * 当前约定：`0` 表示全部未就绪，`1` 表示至少一个通道可用。
 *
 * @return 整体网络状态值。
 */
uint8_t net_transparent_get_status(void);

/**
 * @brief 获取指定通道的串口上行队列。
 *
 * @param[in] port 目标通道。
 * @return 队列句柄。
 */
QueueHandle_t net_transparent_get_uart_rx_queue(uint8_t port);

/**
 * @brief 将串口数据投递到网络发送队列。
 *
 * @param[in] data 待发送的数据对象。
 */
void net_transparent_send_to_network(const net_data_t *data);

/**
 * @brief 将网络数据投递到串口发送队列。
 *
 * @param[in] port 目标串口通道。
 * @param[in] data 数据缓冲区。
 * @param[in] len 数据长度。
 */
void net_transparent_send_to_uart(uint8_t port, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
