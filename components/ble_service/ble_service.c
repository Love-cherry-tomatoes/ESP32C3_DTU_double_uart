#include "ble_service.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "cmd_handler.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BLE_SRV";
/* BLE 主要承担轻量配置、状态读取和命令下发入口。 */

#define GATTS_SERVICE_UUID_CONFIG    0xA5C3
#define GATTS_CHAR_UUID_CFG_READ     0x0002
#define GATTS_CHAR_UUID_CFG_WRITE    0x0003
#define GATTS_CHAR_UUID_CFG_APPLY    0x0004
#define GATTS_SERVICE_UUID_STATUS    0xA5C4
#define GATTS_CHAR_UUID_WIFI_ST      0x0011
#define GATTS_CHAR_UUID_NET_ST       0x0012
#define GATTS_CHAR_UUID_MQTT_ST      0x0013
#define GATTS_SERVICE_UUID_LOG       0xA5C5
#define GATTS_CHAR_UUID_LOG_READ     0x0021
#define GATTS_CHAR_UUID_LOG_CTRL     0x0022
#define GATTS_SERVICE_UUID_CMD       0xA5C6
#define GATTS_CHAR_UUID_CMD_REQ      0x0031
#define GATTS_CHAR_UUID_CMD_RESP     0x0032

#define BLE_DEVICE_NAME "ESP32C3_DTU"

static uint16_t s_conn_id = 0;
static bool s_connected = false;
static esp_gatt_if_t s_gatts_if = 0;

enum {
    /* 属性句柄索引表，便于在回调中快速判断当前操作的是哪一个特征值。 */
    IDX_CFG_SVC,
    IDX_CFG_READ_CHAR, IDX_CFG_READ_VAL,
    IDX_CFG_WRITE_CHAR, IDX_CFG_WRITE_VAL,
    IDX_CFG_APPLY_CHAR, IDX_CFG_APPLY_VAL,

    IDX_STA_SVC,
    IDX_STA_WIFI_CHAR, IDX_STA_WIFI_VAL, IDX_STA_WIFI_CFG,
    IDX_STA_NET_CHAR, IDX_STA_NET_VAL, IDX_STA_NET_CFG,
    IDX_STA_MQTT_CHAR, IDX_STA_MQTT_VAL, IDX_STA_MQTT_CFG,

    IDX_LOG_SVC,
    IDX_LOG_READ_CHAR, IDX_LOG_READ_VAL, IDX_LOG_READ_CFG,
    IDX_LOG_CTRL_CHAR, IDX_LOG_CTRL_VAL,

    IDX_CMD_SVC,
    IDX_CMD_REQ_CHAR, IDX_CMD_REQ_VAL,
    IDX_CMD_RESP_CHAR, IDX_CMD_RESP_VAL, IDX_CMD_RESP_CFG,

    IDX_NB,
};

static uint16_t s_handle_table[IDX_NB];

static uint8_t s_adv_config_done = 0;

static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,
    0x03, 0x02, 0xC3, 0xA5,
};

static uint8_t s_adv_scan_resp[] = {
    0x0E, 0x09, 'E','S','P','3','2','C','3','_','D','T','U','_','0','0'
};

static const esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void send_simple_read_response(esp_gatt_if_t gatts_if, uint16_t conn_id,
                                      uint32_t trans_id, uint16_t handle,
                                      const uint8_t *value, uint16_t len)
{
    /* 对简单只读特征值统一封装响应，减少重复代码。 */
    esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.attr_value.handle = handle;
    rsp.attr_value.len = len;
    if (value && len > 0) {
        memcpy(rsp.attr_value.value, value, len);
    }
    esp_ble_gatts_send_response(gatts_if, conn_id, trans_id, ESP_GATT_OK, &rsp);
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* GAP 侧主要负责广播配置和广播启动时机。 */
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= ~1;
        if (s_adv_config_done == 0) {
            esp_ble_gap_start_advertising((esp_ble_adv_params_t *)&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= ~2;
        if (s_adv_config_done == 0) {
            esp_ble_gap_start_advertising((esp_ble_adv_params_t *)&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "adv start: %s", param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "ok" : "fail");
        break;
    default:
        break;
    }
}

static void handle_cfg_write(uint16_t len, const uint8_t *value)
{
    if (!value || len == 0) return;
    char *str = malloc(len + 1);
    if (!str) return;
    memcpy(str, value, len);
    str[len] = '\0';

    /* 当前 BLE 配置写入采用最简单的 key=value 文本协议。 */
    char *eq = strchr(str, '=');
    if (eq) {
        *eq = '\0';
        char *key = str;
        char *val = eq + 1;
        dtu_config_set_str(key, val);
        DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "BLE config: %s=%s", key, val);
    }
    free(str);
}

static void handle_cfg_apply(void)
{
    /* BLE 侧修改配置后同样通过重启来统一生效。 */
    DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "BLE apply config -> restart");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_cmd_req(uint16_t len, const uint8_t *value)
{
    /* 直接复用命令处理器，避免 BLE 自己再维护一套命令实现。 */
    if (!value || len == 0) return;
    cmd_handler_submit(CMD_SRC_BLE, value, len);
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    /* GATTS 事件负责具体读写行为和连接状态维护。 */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s_gatts_if = gatts_if;
        } else {
            DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "GATT reg failed, status=%d", param->reg.status);
            return;
        }
    }

    switch (event) {
    case ESP_GATTS_REG_EVT:
        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);
        esp_ble_gap_config_adv_data_raw(s_adv_data, sizeof(s_adv_data));
        esp_ble_gap_config_scan_rsp_data_raw(s_adv_scan_resp, sizeof(s_adv_scan_resp));
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_handle_table[IDX_CFG_READ_VAL]) {
            /* 配置读取返回摘要字符串，而不是完整 JSON，保持 BLE 载荷轻量。 */
            dtu_config_t cfg;
            dtu_config_get(&cfg);
            char resp[256];
            snprintf(resp, sizeof(resp), "ssid=%s,s1=%s:%u,s2=%s:%u,m1=%d,m2=%d",
                     cfg.wifi_ssid, cfg.srv1_ip, cfg.srv1_port,
                     cfg.srv2_ip, cfg.srv2_port, cfg.uart1_mode, cfg.uart2_mode);
            send_simple_read_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                      s_handle_table[IDX_CFG_READ_VAL],
                                      (const uint8_t *)resp, (uint16_t)strlen(resp));
        } else if (param->read.handle == s_handle_table[IDX_STA_WIFI_VAL]) {
            uint8_t st = 0;
            send_simple_read_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                      s_handle_table[IDX_STA_WIFI_VAL], &st, sizeof(st));
        } else if (param->read.handle == s_handle_table[IDX_LOG_READ_VAL]) {
            blackbox_entry_t entries[10];
            int n = blackbox_read_recent(entries, 10);
            char resp[512] = {0};
            for (int i = 0; i < n && strlen(resp) < 480; i++) {
                char buf[64];
                blackbox_format_entry(&entries[i], buf, sizeof(buf));
                strcat(resp, buf);
                strcat(resp, "\n");
            }
            send_simple_read_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                      s_handle_table[IDX_LOG_READ_VAL],
                                      (const uint8_t *)resp, (uint16_t)strlen(resp));
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle_table[IDX_CFG_WRITE_VAL]) {
            handle_cfg_write(param->write.len, param->write.value);
        } else if (param->write.handle == s_handle_table[IDX_CFG_APPLY_VAL]) {
            handle_cfg_apply();
        } else if (param->write.handle == s_handle_table[IDX_CMD_REQ_VAL]) {
            handle_cmd_req(param->write.len, param->write.value);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "BLE client connected");
        esp_ble_gap_stop_advertising();
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "BLE client disconnected");
        esp_ble_gap_start_advertising((esp_ble_adv_params_t *)&s_adv_params);
        break;

    case ESP_GATTS_CREATE_EVT:
        if (param->create.service_id.id.inst_id < IDX_NB) {
            s_handle_table[param->create.service_id.id.inst_id] = param->create.service_handle;
        }
        esp_ble_gatts_start_service(param->create.service_handle);
        break;

    default:
        break;
    }
}

void ble_service_notify_status(void)
{
    if (!s_connected) return;
    /* 当前仅预留接口，后续可在这里补充 notify 推送逻辑。 */
}

esp_err_t ble_service_init(void)
{
    /* 仅释放经典蓝牙内存，保留 BLE 模式以节省 RAM。 */
    esp_err_t ret;

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        DTU_LOGW(DTU_LOG_MOD_BLE, TAG, "BTDM mem release failed (expected if already released)");
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "GATTS callback register failed");
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "GAP callback register failed");
        return ret;
    }

    ret = esp_ble_gatt_set_local_mtu(512);
    if (ret) {
        DTU_LOGW(DTU_LOG_MOD_BLE, TAG, "set MTU failed");
    }

    s_adv_config_done = 3;

    ret = esp_ble_gatts_app_register(0);
    if (ret) {
        DTU_LOGE(DTU_LOG_MOD_BLE, TAG, "GATTS app register failed");
        return ret;
    }

    DTU_LOGI(DTU_LOG_MOD_BLE, TAG, "BLE service init done");
    return ESP_OK;
}
