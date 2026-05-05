#include "dtu_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "DTU_LOG";
/* 每个模块对应一个开关位，启动时默认全部开启。 */
static bool s_mod_enabled[DTU_LOG_MOD_MAX];
static const char *NVS_NAMESPACE = "dtu_log";
static const char *NVS_KEY = "mod_mask";

static const char *s_mod_names[DTU_LOG_MOD_MAX] = {
    "MAIN", "WIFI", "RS485", "NET", "WEB",
    "BLE", "PT1000", "CMD", "OTA", "CONFIG", "BLACKBOX"
};

void dtu_log_init(void)
{
    /* 默认先全开，再尝试用 NVS 中的开关掩码覆盖。 */
    memset(s_mod_enabled, true, sizeof(s_mod_enabled));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint32_t mask = 0xFFFFFFFF;
        nvs_get_u32(handle, NVS_KEY, &mask);
        nvs_close(handle);
        /* NVS 中每一位表示一个模块是否启用日志。 */
        for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
            s_mod_enabled[i] = (mask & (1 << i)) != 0;
        }
    }

    ESP_LOGI(TAG, "dtu_log init done");
    for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
        ESP_LOGI(TAG, "  %s: %s", s_mod_names[i], s_mod_enabled[i] ? "ON" : "OFF");
    }
}

void dtu_log_enable(dtu_log_mod_t mod, bool enable)
{
    if (mod >= DTU_LOG_MOD_MAX) return;
    s_mod_enabled[mod] = enable;

    /* 每次调整单个模块开关后都重新计算并持久化整个位图。 */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint32_t mask = 0;
        for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
            if (s_mod_enabled[i]) mask |= (1 << i);
        }
        nvs_set_u32(handle, NVS_KEY, mask);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool dtu_log_is_enabled(dtu_log_mod_t mod)
{
    if (mod >= DTU_LOG_MOD_MAX) return false;
    return s_mod_enabled[mod];
}

void dtu_log_enable_all(bool enable)
{
    /* 提供全开/全关快捷入口，便于远程命令统一控制日志量。 */
    for (int i = 0; i < DTU_LOG_MOD_MAX; i++) {
        s_mod_enabled[i] = enable;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint32_t mask = enable ? 0xFFFFFFFF : 0;
        nvs_set_u32(handle, NVS_KEY, mask);
        nvs_commit(handle);
        nvs_close(handle);
    }
}
