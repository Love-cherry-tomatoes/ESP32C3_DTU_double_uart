#include "pt1000_adc.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "dtu_log.h"
#include "blackbox.h"

#define TAG                         "pt1000_adc"

#define NVS_NAMESPACE              "dtu_cal"
#define NVS_KEY_OFFSET             "pt1000_offset"
#define NVS_KEY_CAL_DONE           "pt1000_cal_done"
#define NVS_KEY_CAL_RAW            "pt1000_cal_raw"

#define PT1000_ADC_UNIT            ADC_UNIT_1
#define PT1000_ADC_ATTEN           ADC_ATTEN_DB_12
#define PT1000_ADC_BITWIDTH        ADC_BITWIDTH_12

#define PT1000_R0                  1000.0f          // PT1000 resistance at 0°C
#define PT1000_VCC                 3.3f             // Supply voltage
#define PT1000_ADC_MAX             4096.0f          // 12-bit ADC full scale

#define PT1000_NUM_SAMPLES         16               // Multi-sample count

/* PT1000 校准过程写入黑匣子的事件编号。 */
#define BB_EVT_PT1000_CAL_START    0
#define BB_EVT_PT1000_CAL_DONE     1
#define BB_EVT_PT1000_CAL_FAIL     2

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static int32_t s_offset = 0;
static bool s_initialized = false;

static esp_err_t pt1000_adc_load_offset(void)
{
    /* 校准偏移单独存储，避免重启后丢失现场标定结果。 */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "NVS open failed, using default offset 0");
        s_offset = 0;
        return ESP_OK;
    }

    err = nvs_get_i32(nvs, NVS_KEY_OFFSET, &s_offset);
    if (err != ESP_OK) {
        DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "Offset key not found, using default 0");
        s_offset = 0;
    } else {
        DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "Loaded offset from NVS: %ld", (long)s_offset);
    }

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t pt1000_adc_save_offset(int32_t offset, int raw)
{
    /* 同时保存 offset、是否已校准以及校准时原始 ADC 值，便于追溯。 */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to open NVS for writing: 0x%x", err);
        return err;
    }

    err = nvs_set_i32(nvs, NVS_KEY_OFFSET, offset);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to save offset: 0x%x", err);
        nvs_close(nvs);
        return err;
    }

    uint8_t cal_done = 1;
    err = nvs_set_u8(nvs, NVS_KEY_CAL_DONE, cal_done);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to save cal_done: 0x%x", err);
        nvs_close(nvs);
        return err;
    }

    err = nvs_set_i32(nvs, NVS_KEY_CAL_RAW, (int32_t)raw);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to save cal_raw: 0x%x", err);
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to commit NVS: 0x%x", err);
    }

    nvs_close(nvs);
    return err;
}

esp_err_t pt1000_adc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* 采用 oneshot 模式即可满足周期读取场景，实现也相对简单。 */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = PT1000_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to init ADC unit: 0x%x", err);
        return err;
    }

    /* 这里仅配置单个采样通道。 */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = PT1000_ADC_ATTEN,
        .bitwidth = PT1000_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_handle, PT1000_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to config ADC channel: 0x%x", err);
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    /* 装载历史校准偏移，保证掉电后读数口径一致。 */
    pt1000_adc_load_offset();

    s_initialized = true;
    DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "PT1000 ADC initialized (GPIO%d, CH%d, offset=%ld)",
             PT1000_ADC_GPIO, PT1000_ADC_CHANNEL, (long)s_offset);

    return ESP_OK;
}

int pt1000_adc_read_raw(void)
{
    if (!s_initialized || !s_adc_handle) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "ADC not initialized");
        return 0;
    }

    int sum = 0;
    int valid = 0;

    /* 多次采样取平均，降低瞬时抖动。 */
    for (int i = 0; i < PT1000_NUM_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_handle, PT1000_ADC_CHANNEL, &raw);
        if (err == ESP_OK) {
            sum += raw;
            valid++;
        } else {
            DTU_LOGW(DTU_LOG_MOD_PT1000, TAG, "ADC read failed at sample %d: 0x%x", i, err);
        }
    }

    if (valid == 0) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "All ADC samples failed");
        return 0;
    }

    return sum / valid;
}

float pt1000_adc_read_temperature(void)
{
    int raw = pt1000_adc_read_raw();
    if (raw <= 0) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Invalid ADC raw value: %d", raw);
        return -999.0f;
    }

    /* 先由 ADC 原始值换算出分压点电压。 */
    float v_adc = (float)raw * PT1000_VCC / PT1000_ADC_MAX;

    /* 分压公式换算 PT1000 阻值。 */
    float r_pt1000;
    float v_diff = PT1000_VCC - v_adc;
    if (v_diff <= 0.001f) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "VCC - V_adc too small: %.3f", v_diff);
        return -999.0f;
    }
    r_pt1000 = PT1000_R_REF * v_adc / v_diff;

    /* 使用线性近似把阻值转换为温度。 */
    float temperature = (r_pt1000 - PT1000_R0) / (PT1000_R0 * PT1000_ALPHA);

    /* 最后叠加现场校准偏移。 */
    temperature += (float)s_offset;

    return temperature;
}

esp_err_t pt1000_adc_calibrate(int32_t ref_ohm)
{
    if (!s_initialized) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "ADC not initialized, cannot calibrate");
        return ESP_ERR_INVALID_STATE;
    }

    DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "Calibration started with ref_ohm=%ld", (long)ref_ohm);
    BB_INFO(BB_CAT_PT1000, BB_EVT_PT1000_CAL_START, ref_ohm, "cal_start");

    /* 读取当前平均 ADC 值作为校准基准。 */
    int raw = pt1000_adc_read_raw();
    DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "Calibration raw ADC: %d", raw);

    if (raw <= 0) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Invalid raw value during calibration");
        BB_INFO(BB_CAT_PT1000, BB_EVT_PT1000_CAL_FAIL, raw, "cal_fail");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 先反推出当前实测阻值。 */
    float v_adc = (float)raw * PT1000_VCC / PT1000_ADC_MAX;
    float v_diff = PT1000_VCC - v_adc;
    if (v_diff <= 0.001f) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Invalid voltage divider reading during calibration");
        BB_INFO(BB_CAT_PT1000, BB_EVT_PT1000_CAL_FAIL, raw, "cal_fail");
        return ESP_ERR_INVALID_RESPONSE;
    }
    float r_measured = PT1000_R_REF * v_adc / v_diff;

    /* 标准电阻对应一个理论温度值。 */
    float t_expected = ((float)ref_ohm - PT1000_R0) / (PT1000_R0 * PT1000_ALPHA);

    /* 计算未加 offset 时的实测温度。 */
    float t_measured = (r_measured - PT1000_R0) / (PT1000_R0 * PT1000_ALPHA);

    /* 偏移量定义为：理论温度 - 当前测得温度。 */
    int32_t new_offset = (int32_t)(t_expected - t_measured);

    DTU_LOGI(DTU_LOG_MOD_PT1000, TAG,
             "Calibration: raw=%d, R_meas=%.1f, T_exp=%.2f, T_meas=%.2f, offset=%ld",
             raw, r_measured, t_expected, t_measured, (long)new_offset);

    /* Save to NVS */
    esp_err_t err = pt1000_adc_save_offset(new_offset, raw);
    if (err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_PT1000, TAG, "Failed to save calibration data");
        BB_INFO(BB_CAT_PT1000, BB_EVT_PT1000_CAL_FAIL, new_offset, "cal_fail");
        return err;
    }

    s_offset = new_offset;
    DTU_LOGI(DTU_LOG_MOD_PT1000, TAG, "Calibration complete, new offset=%ld", (long)s_offset);
    BB_INFO(BB_CAT_PT1000, BB_EVT_PT1000_CAL_DONE, new_offset, "cal_done");

    return ESP_OK;
}

int32_t pt1000_adc_get_offset(void)
{
    return s_offset;
}
