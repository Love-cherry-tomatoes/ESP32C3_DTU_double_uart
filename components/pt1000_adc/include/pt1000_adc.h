#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "hal/adc_types.h"
#include <stdint.h>

/** @brief PT1000 所用 ADC 通道。 */
#define PT1000_ADC_CHANNEL      ADC_CHANNEL_3
/** @brief PT1000 所用 GPIO。 */
#define PT1000_ADC_GPIO         GPIO_NUM_3
/** @brief 分压参考电阻阻值。 */
#define PT1000_R_REF            2200.0f
/** @brief PT1000 线性近似温度系数。 */
#define PT1000_ALPHA            0.00385f

/**
 * @brief 初始化 PT1000 ADC 采样模块。
 *
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t pt1000_adc_init(void);

/**
 * @brief 读取当前温度。
 *
 * @return 当前温度，单位为摄氏度；若采样异常则返回明显异常值。
 */
float pt1000_adc_read_temperature(void);

/**
 * @brief 读取当前原始 ADC 值。
 *
 * 该值通常为多次采样后的平均值。
 *
 * @return 原始 ADC 值。
 */
int pt1000_adc_read_raw(void);

/**
 * @brief 执行 PT1000 校准。
 *
 * @param[in] ref_ohm 当前接入的参考电阻阻值。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t pt1000_adc_calibrate(int32_t ref_ohm);

/**
 * @brief 获取当前温度偏移量。
 *
 * @return 当前偏移量。
 */
int32_t pt1000_adc_get_offset(void);
