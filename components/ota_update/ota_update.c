#include "ota_update.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OTA";
/* OTA 采用独立任务执行，避免阻塞调用方。 */
static bool s_in_progress = false;
static int s_progress = 0;
static TaskHandle_t s_ota_task_handle = NULL;

static void ota_task(void *arg)
{
    /* URL 由启动函数复制后传入，这里拥有其释放责任。 */
    char *url = (char *)arg;
    s_in_progress = true;
    s_progress = 0;

    DTU_LOGI(DTU_LOG_MOD_OTA, TAG, "OTA start: %s", url);
    BB_INFO(BB_CAT_OTA, 0, 0, "ota_start");

    /* 当前使用 HTTPS OTA 封装流程，证书校验暂未启用。 */
    esp_http_client_config_t http_config = {
        .url = url,
        .cert_pem = NULL,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        s_progress = 100;
        DTU_LOGI(DTU_LOG_MOD_OTA, TAG, "OTA success, restarting...");
        BB_INFO(BB_CAT_OTA, 1, 100, "ota_ok");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        DTU_LOGE(DTU_LOG_MOD_OTA, TAG, "OTA failed: %s", esp_err_to_name(ret));
        BB_ERROR(BB_CAT_OTA, 2, ret, "ota_fail");
    }

    free(url);
    s_in_progress = false;
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ota_update_init(void)
{
    /* 如果设备当前运行的是待确认镜像，则在这里标记为有效，取消回滚。 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();

    DTU_LOGI(DTU_LOG_MOD_OTA, TAG, "running: %s offset=0x%lx, boot: %s",
             running->label, running->address, boot->label);

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            DTU_LOGI(DTU_LOG_MOD_OTA, TAG, "OTA image pending verify, marking valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    return ESP_OK;
}

esp_err_t ota_update_start(const char *url)
{
    if (!url || strlen(url) == 0) return ESP_ERR_INVALID_ARG;
    if (s_in_progress) return ESP_ERR_INVALID_STATE;

    /* 拷贝 URL，避免调用方传入的缓冲区在 OTA 任务运行期间失效。 */
    char *url_copy = strdup(url);
    if (!url_copy) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 4096, url_copy, 2, &s_ota_task_handle);
    if (ret != pdPASS) {
        free(url_copy);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

int ota_update_get_progress(void)
{
    return s_progress;
}

bool ota_update_is_in_progress(void)
{
    return s_in_progress;
}
