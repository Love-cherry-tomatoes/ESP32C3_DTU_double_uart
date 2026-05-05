#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "main.h"
#include "dtu_log.h"
#include "dtu_config.h"
#include "blackbox.h"
#include "wifi_mgr.h"
#include "rs485_uart.h"
#include "net_transparent.h"
#include "web_server.h"
#include "ble_service.h"
#include "pt1000_adc.h"
#include "cmd_handler.h"
#include "ota_update.h"

static const char *TAG = "MAIN";

/* 全局事件组用于在各组件之间同步关键系统状态，例如配置加载、WiFi 连接等。 */
EventGroupHandle_t g_dtu_event_group = NULL;
/* 保存启动阶段读取到的配置快照，便于在启动完成后打印启动摘要。 */
static dtu_config_t s_boot_cfg;

static void cmd_response_handler(uint8_t source, const char *response, uint16_t len)
{
    /* 命令处理器本身不关心“响应最终走哪条物理链路”，这里由主控层完成回送映射。 */
    switch (source) {
    case CMD_SRC_TCP0:
        net_transparent_send_to_uart(0, (const uint8_t *)response, len);
        break;
    case CMD_SRC_TCP1:
        net_transparent_send_to_uart(1, (const uint8_t *)response, len);
        break;
    case CMD_SRC_UDP:
    case CMD_SRC_MQTT:
        net_transparent_send_to_uart(0, (const uint8_t *)response, len);
        break;
    case CMD_SRC_BLE:
        // BLE response handled by BLE service
        break;
    default:
        break;
    }
}

static void ota_start_wrapper(const char *url)
{
    /* 通过轻量包装把 OTA 能力注册给其他模块，避免它们直接依赖 ota_update 内部实现。 */
    ota_update_start(url);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32C3 DTU Starting ===");

    /* 1. 初始化 NVS。配置、日志开关、黑匣子等模块都依赖持久化存储。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. 初始化网络接口与默认事件循环，WiFi/Web/BLE 等模块会复用这一基础设施。 */
    esp_netif_init();
    esp_event_loop_create_default();

    /* 3. 创建系统级事件组，用于跨模块同步运行状态。 */
    g_dtu_event_group = xEventGroupCreate();

    /* 4. 初始化可按模块开关的日志系统，尽量让后续初始化阶段都有统一日志输出。 */
    dtu_log_init();

    /* 5. 读取并装载全局配置。后续大多数组件都会从这里读取运行参数。 */
    dtu_config_init();
    xEventGroupSetBits(g_dtu_event_group, EVT_CONFIG_LOADED);

    /* 6. 初始化黑匣子，尽早开始记录系统关键事件，便于现场追溯故障。 */
    blackbox_init();

    /* 7. 启动 WiFi 管理器。其内部会根据配置决定走 STA、AP 或 APSTA。 */
    wifi_mgr_init();

    /* 8. 等待 WiFi 连接结果，但只等待有限时间，避免主流程被永久阻塞。 */
    EventBits_t bits = xEventGroupWaitBits(g_dtu_event_group,
                                            EVT_WIFI_CONNECTED,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(15000));
    if (!(bits & EVT_WIFI_CONNECTED)) {
        DTU_LOGW(DTU_LOG_MOD_MAIN, TAG,
                 "WiFi connection was not confirmed within timeout, status=%u reason=%d (%s)",
                 (unsigned)wifi_mgr_get_status(),
                 wifi_mgr_get_disconnect_reason_code(),
                 wifi_mgr_get_disconnect_reason());
    }

    /* 9. 初始化双路 RS485 串口，本地采集/透传链路从这里开始建立。 */
    rs485_uart_init();

    /* 10. 初始化 PT1000 温度采集模块，供命令接口、Web 接口复用。 */
    pt1000_adc_init();

    /* 11. 初始化命令处理器，并将温度、OTA 等具体业务能力以回调形式注入进去。 */
    cmd_handler_init();
    cmd_handler_set_response_cb(cmd_response_handler);
    cmd_handler_set_pt1000_funcs(pt1000_adc_read_temperature, pt1000_adc_calibrate);
    cmd_handler_set_ota_func(ota_start_wrapper);

    /* 12. 初始化 Web 配置与运维入口。 */
    web_server_set_pt1000_funcs(pt1000_adc_read_temperature, pt1000_adc_calibrate);
    web_server_set_ota_func(ota_start_wrapper);
    esp_err_t web_err = web_server_init();
    if (web_err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_WEB, TAG, "web_server_init failed: %s", esp_err_to_name(web_err));
    }

    /* 13. 初始化 BLE，作为近场维护与配置入口。 */
    DTU_LOGI(DTU_LOG_MOD_MAIN, TAG, "Init BLE, free heap before=%u", (unsigned)esp_get_free_heap_size());
    esp_err_t ble_err = ble_service_init();
    if (ble_err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "ble_service_init failed: %s", esp_err_to_name(ble_err));
    }

    /* 14. 初始化透明传输引擎，将串口与 TCP/UDP/MQTT/HTTP 通道连接起来。 */
    esp_err_t net_err = net_transparent_init();
    if (net_err != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_NET, TAG, "net_transparent_init failed: %s", esp_err_to_name(net_err));
    }

    /* 15. 初始化 OTA 管理模块，用于版本确认与在线升级。 */
    ota_update_init();

    /* 16. 打印启动后的配置摘要，方便现场确认当前运行参数。 */
    dtu_config_get(&s_boot_cfg);
    ESP_LOGI(TAG, "=== DTU Ready ===");
    ESP_LOGI(TAG, "  WiFi: %s (mode=%d)", s_boot_cfg.wifi_ssid, s_boot_cfg.wifi_mode);
    ESP_LOGI(TAG, "  Socket1: %s:%u", s_boot_cfg.srv1_ip, s_boot_cfg.srv1_port);
    ESP_LOGI(TAG, "  Socket2: %s:%u", s_boot_cfg.srv2_ip, s_boot_cfg.srv2_port);
    ESP_LOGI(TAG, "  Modes: uart1=%u uart2=%u", s_boot_cfg.uart1_mode, s_boot_cfg.uart2_mode);
    ESP_LOGI(TAG, "  UART1: %" PRIu32 " baud", s_boot_cfg.uart1_baud);
    ESP_LOGI(TAG, "  UART2: %" PRIu32 " baud", s_boot_cfg.uart2_baud);
    ESP_LOGI(TAG, "  Free heap: %u", (unsigned)esp_get_free_heap_size());

    BB_INFO(BB_CAT_SYSTEM, BB_EVT_SYS_BOOT, esp_get_free_heap_size(), "ready");

    /* app_main 只负责系统编排。各子模块初始化完成后即可退出主任务。 */
    vTaskDelete(NULL);
}
