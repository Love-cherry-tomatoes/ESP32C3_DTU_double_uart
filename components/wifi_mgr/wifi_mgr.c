#include "wifi_mgr.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "dtu_events.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_mgr";
/* 
 * WiFi 管理器负责三件事：
 * 1. 根据配置启动 STA/AP/APSTA；
 * 2. 处理断线重连与失败回退；
 * 3. 将关键网络状态同步到全局事件组。
 */

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

static esp_netif_t       *s_sta_netif   = NULL;
static esp_netif_t       *s_ap_netif    = NULL;
static EventGroupHandle_t s_event_group  = NULL;

static volatile uint8_t   s_status       = WIFI_MGR_STATUS_DISCONNECTED;
static volatile int       s_reason_code  = 0;
static volatile int       s_retry_count  = 0;

#define STA_CONNECT_BIT    BIT0
#define STA_FAIL_BIT       BIT1
#define STA_GOT_IP_BIT     BIT2
#define MAX_RETRY_COUNT    5

static void sync_global_wifi_bits(EventBits_t set_bits, EventBits_t clear_bits)
{
    /* 组件内部状态变化后，同时更新系统级事件位，方便 main 和其他模块等待。 */
    if (!g_dtu_event_group) {
        return;
    }

    if (clear_bits) {
        xEventGroupClearBits(g_dtu_event_group, clear_bits);
    }
    if (set_bits) {
        xEventGroupSetBits(g_dtu_event_group, set_bits);
    }
}

/* ------------------------------------------------------------------ */
/*  Disconnect reason strings                                          */
/* ------------------------------------------------------------------ */

static const char *disconnect_reason_str(int code)
{
    /* 仅保留现场最常见的一批断线原因，便于日志直接可读。 */
    switch (code) {
        case WIFI_REASON_UNSPECIFIED:                return "unspecified";
        case WIFI_REASON_AUTH_EXPIRE:                return "auth_expire";
        case WIFI_REASON_AUTH_LEAVE:                 return "auth_leave";
        case WIFI_REASON_ASSOC_EXPIRE:               return "assoc_expire";
        case WIFI_REASON_ASSOC_TOOMANY:              return "assoc_toomany";
        case WIFI_REASON_NOT_AUTHED:                 return "not_authed";
        case WIFI_REASON_NOT_ASSOCED:                return "not_assoced";
        case WIFI_REASON_ASSOC_LEAVE:                return "assoc_leave";
        case WIFI_REASON_ASSOC_NOT_AUTHED:           return "assoc_not_authed";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:        return "disassoc_pwrcap_bad";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:       return "disassoc_supchan_bad";
        case WIFI_REASON_IE_INVALID:                 return "ie_invalid";
        case WIFI_REASON_MIC_FAILURE:                return "mic_failure";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:     return "4way_handshake_timeout";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:   return "group_key_update_timeout";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:         return "ie_in_4way_differs";
        case WIFI_REASON_GROUP_CIPHER_INVALID:       return "group_cipher_invalid";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:    return "pairwise_cipher_invalid";
        case WIFI_REASON_AKMP_INVALID:               return "akmp_invalid";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:      return "unsupp_rsn_ie_version";
        case WIFI_REASON_INVALID_RSN_IE_CAP:         return "invalid_rsn_ie_cap";
        case WIFI_REASON_802_1X_AUTH_FAILED:         return "802_1x_auth_failed";
        case WIFI_REASON_CIPHER_SUITE_REJECTED:      return "cipher_suite_rejected";
        case WIFI_REASON_INVALID_PMKID:              return "invalid_pmkid";
        case WIFI_REASON_BEACON_TIMEOUT:             return "beacon_timeout";
        case WIFI_REASON_NO_AP_FOUND:                return "no_ap_found";
        case WIFI_REASON_AUTH_FAIL:                  return "auth_fail";
        case WIFI_REASON_ASSOC_FAIL:                 return "assoc_fail";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:          return "handshake_timeout";
        case WIFI_REASON_CONNECTION_FAIL:            return "connection_fail";
        case WIFI_REASON_AP_TSF_RESET:               return "ap_tsf_reset";
        case WIFI_REASON_ROAMING:                    return "roaming";
        default:                                     return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Public getters                                                     */
/* ------------------------------------------------------------------ */

uint8_t wifi_mgr_get_status(void)
{
    return s_status;
}

const char *wifi_mgr_get_disconnect_reason(void)
{
    return disconnect_reason_str(s_reason_code);
}

int wifi_mgr_get_disconnect_reason_code(void)
{
    return s_reason_code;
}

/* ------------------------------------------------------------------ */
/*  Event handlers                                                     */
/* ------------------------------------------------------------------ */

static bool is_auth_error(int reason)
{
    /* 认证类错误通常意味着账号、密码或安全参数有问题。 */
    return (reason == WIFI_REASON_AUTH_EXPIRE        ||
            reason == WIFI_REASON_AUTH_LEAVE          ||
            reason == WIFI_REASON_NOT_AUTHED          ||
            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT ||
            reason == WIFI_REASON_IE_IN_4WAY_DIFFERS  ||
            reason == WIFI_REASON_GROUP_CIPHER_INVALID ||
            reason == WIFI_REASON_PAIRWISE_CIPHER_INVALID ||
            reason == WIFI_REASON_AKMP_INVALID        ||
            reason == WIFI_REASON_UNSUPP_RSN_IE_VERSION ||
            reason == WIFI_REASON_INVALID_RSN_IE_CAP  ||
            reason == WIFI_REASON_802_1X_AUTH_FAILED  ||
            reason == WIFI_REASON_CIPHER_SUITE_REJECTED ||
            reason == WIFI_REASON_INVALID_PMKID       ||
            reason == WIFI_REASON_AUTH_FAIL           ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    /* WiFi 事件主要负责连接状态机和失败回退逻辑。 */
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "STA started, connecting...");
            s_status = WIFI_MGR_STATUS_CONNECTING;
            sync_global_wifi_bits(EVT_WIFI_DISCONNECTED, EVT_WIFI_CONNECTED);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *evt =
                (wifi_event_sta_disconnected_t *)event_data;
            s_reason_code = evt->reason;
            DTU_LOGE(DTU_LOG_MOD_WIFI, TAG, "STA disconnected, reason=%d (%s)",
                     evt->reason, disconnect_reason_str(evt->reason));
            BB_ERROR(BB_CAT_WIFI, BB_EVT_WIFI_DISCONNECTED, evt->reason, "disconnected");
            sync_global_wifi_bits(EVT_WIFI_DISCONNECTED, EVT_WIFI_CONNECTED);

            if (is_auth_error(evt->reason)) {
                BB_ERROR(BB_CAT_WIFI, BB_EVT_WIFI_AUTH_ERROR, evt->reason, "auth_err");
            }

            if (s_retry_count < MAX_RETRY_COUNT) {
                s_retry_count++;
                DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Retrying connection (%d/%d)...",
                         s_retry_count, MAX_RETRY_COUNT);
                s_status = WIFI_MGR_STATUS_CONNECTING;
                esp_wifi_connect();
            } else {
                DTU_LOGE(DTU_LOG_MOD_WIFI, TAG,
                         "Max retries reached, falling back to AP mode");
                s_status = WIFI_MGR_STATUS_FAILED;
                xEventGroupSetBits(s_event_group, STA_FAIL_BIT);
                sync_global_wifi_bits(EVT_WIFI_DISCONNECTED, EVT_WIFI_CONNECTED);
                wifi_mgr_start_ap();
            }
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "STA connected to AP");
            s_retry_count = 0;
            break;

        case WIFI_EVENT_AP_START:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "AP started");
            break;

        case WIFI_EVENT_AP_STOP:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "AP stopped");
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Station connected to AP");
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Station disconnected from AP");
            break;

        default:
            break;
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    /* IP 事件用于判断“真正联网可用”的时间点。 */
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_status = WIFI_MGR_STATUS_CONNECTED;
        s_retry_count = 0;
        BB_INFO(BB_CAT_WIFI, BB_EVT_WIFI_CONNECTED, 0, "connected");
        xEventGroupSetBits(s_event_group, STA_GOT_IP_BIT);
        sync_global_wifi_bits(EVT_WIFI_CONNECTED, EVT_WIFI_DISCONNECTED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        DTU_LOGW(DTU_LOG_MOD_WIFI, TAG, "Lost IP address");
        s_status = WIFI_MGR_STATUS_CONNECTING;
        sync_global_wifi_bits(EVT_WIFI_DISCONNECTED, EVT_WIFI_CONNECTED);
    }
}

/* ------------------------------------------------------------------ */
/*  AP mode helpers                                                    */
/* ------------------------------------------------------------------ */

esp_err_t wifi_mgr_start_ap(void)
{
    /* AP 名称带上 MAC 后缀，方便现场区分多台设备。 */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    char ap_ssid[24];
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32C3_DTU_%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Starting AP mode, SSID=%s", ap_ssid);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, "12345678", sizeof(ap_config.ap.password));
    ap_config.ap.channel     = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode    = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  STA connect                                                        */
/* ------------------------------------------------------------------ */

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass)
{
    if (!ssid) {
        DTU_LOGE(DTU_LOG_MOD_WIFI, TAG, "SSID is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Connecting to SSID=%s", ssid);

    /* 这里只关心基础 STA 参数，复杂企业认证场景当前未覆盖。 */
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    if (pass) {
        strlcpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
    }
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_retry_count = 0;
    s_status = WIFI_MGR_STATUS_CONNECTING;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

esp_err_t wifi_mgr_init(void)
{
    DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "Initializing WiFi manager");

    /* 管理器内部也保留一份事件组，用于本模块内部等待与同步。 */
    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        DTU_LOGE(DTU_LOG_MOD_WIFI, TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize netif */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default netif instances */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    /* 通过统一配置源决定启动路径。 */
    dtu_config_t dtu_cfg;
    esp_err_t ret = dtu_config_get(&dtu_cfg);
    if (ret != ESP_OK) {
        DTU_LOGE(DTU_LOG_MOD_WIFI, TAG, "Failed to get DTU config, starting AP mode");
        wifi_mgr_start_ap();
        return ESP_OK;
    }

    switch (dtu_cfg.wifi_mode) {
    case DTU_WIFI_MODE_STA:
        DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "WiFi mode: STA");
        wifi_mgr_connect(dtu_cfg.wifi_ssid, dtu_cfg.wifi_pass);
        break;

    case DTU_WIFI_MODE_AP:
        DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "WiFi mode: AP");
        wifi_mgr_start_ap();
        break;

    case DTU_WIFI_MODE_APSTA:
        DTU_LOGI(DTU_LOG_MOD_WIFI, TAG, "WiFi mode: APSTA");
        wifi_mgr_connect(dtu_cfg.wifi_ssid, dtu_cfg.wifi_pass);
        /* AP will also be started; wifi_mgr_connect sets STA mode,
           wifi_mgr_start_ap will switch to APSTA and start AP */
        wifi_mgr_start_ap();
        break;

    default:
        DTU_LOGW(DTU_LOG_MOD_WIFI, TAG,
                 "Unknown WiFi mode %d, falling back to AP", dtu_cfg.wifi_mode);
        wifi_mgr_start_ap();
        break;
    }

    return ESP_OK;
}
