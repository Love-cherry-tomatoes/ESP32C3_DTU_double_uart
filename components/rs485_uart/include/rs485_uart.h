#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief RS485-A 对应的 UART 号。 */
#define RS485_A_UART_NUM        UART_NUM_1
/** @brief RS485-A TX 引脚。 */
#define RS485_A_TX_PIN          GPIO_NUM_10
/** @brief RS485-A RX 引脚。 */
#define RS485_A_RX_PIN          GPIO_NUM_5
/** @brief RS485-A DE/RE 控制引脚。 */
#define RS485_A_DE_RE_PIN       GPIO_NUM_6

/** @brief RS485-B 对应的 UART 号。 */
#define RS485_B_UART_NUM        UART_NUM_0
/** @brief RS485-B TX 引脚。 */
#define RS485_B_TX_PIN          GPIO_NUM_21
/** @brief RS485-B RX 引脚。 */
#define RS485_B_RX_PIN          GPIO_NUM_20
/** @brief RS485-B DE/RE 控制引脚。 */
#define RS485_B_DE_RE_PIN       GPIO_NUM_4

/** @brief 单帧 RS485 数据最大长度。 */
#define RS485_MAX_DATA_LEN      256

/**
 * @brief RS485 数据封装。
 */
typedef struct {
    uint8_t  port;
    uint8_t  data[RS485_MAX_DATA_LEN];
    uint16_t len;
} rs485_data_t;

/**
 * @brief 初始化双路 RS485 串口。
 *
 * 会根据 `dtu_config` 中的串口参数完成配置，并创建接收任务和接收队列。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t rs485_uart_init(void);

/**
 * @brief 发送一帧 RS485 数据。
 *
 * 内部会自动完成 DE/RE 方向切换。
 *
 * @param[in] port 目标串口，`0` 表示 RS485-A，`1` 表示 RS485-B。
 * @param[in] data 数据缓冲区。
 * @param[in] len 数据长度。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t rs485_uart_send(uint8_t port, const uint8_t *data, uint16_t len);

/**
 * @brief 获取指定串口的接收队列。
 *
 * @param[in] port 目标串口，`0` 表示 RS485-A，`1` 表示 RS485-B。
 * @return 队列句柄；若端口无效或未初始化则返回 `NULL`。
 */
QueueHandle_t rs485_uart_get_rx_queue(uint8_t port);

/**
 * @brief 动态修改指定串口的波特率。
 *
 * @param[in] port 目标串口。
 * @param[in] baud 新波特率。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t rs485_uart_set_baud(uint8_t port, uint32_t baud);

/**
 * @brief 完整重配置指定串口。
 *
 * @param[in] port 目标串口。
 * @param[in] baud 新波特率。
 * @param[in] parity 校验位，取值见 `DTU_PARITY_*`。
 * @param[in] stop_bits 停止位数量，通常为 `1` 或 `2`。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t rs485_uart_set_config(uint8_t port, uint32_t baud, uint8_t parity, uint8_t stop_bits);

#ifdef __cplusplus
}
#endif
