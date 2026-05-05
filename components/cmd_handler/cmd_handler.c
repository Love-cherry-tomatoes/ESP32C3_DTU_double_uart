#include "cmd_handler.h"
#include "dtu_config.h"
#include "dtu_log.h"
#include "blackbox.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CMD";

/* 命令请求统一进入队列，由专用任务异步处理，避免阻塞串口或网络回调。 */
typedef struct {
    uint8_t  source;
    uint8_t  data[512];
    uint16_t len;
} cmd_request_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_cmd_task_handle = NULL;
static cmd_response_cb_t s_response_cb = NULL;

/* 由 main.c 在启动阶段注入的业务函数。 */
static float (*s_pt1000_read_temp)(void) = NULL;
static esp_err_t (*s_pt1000_calibrate)(int32_t ref_ohm) = NULL;
static void (*s_ota_start)(const char *url) = NULL;

void cmd_handler_set_pt1000_funcs(float (*read_temp)(void), esp_err_t (*calibrate)(int32_t))
{
    s_pt1000_read_temp = read_temp;
    s_pt1000_calibrate = calibrate;
}

void cmd_handler_set_ota_func(void (*ota_start)(const char *url))
{
    s_ota_start = ota_start;
}

static void send_response(uint8_t source, const char *resp)
{
    /* 命令模块不直接感知底层通道，只通过回调把响应交给上层。 */
    if (s_response_cb) {
        s_response_cb(source, resp, (uint16_t)strlen(resp));
    }
}

static void cmd_read_temp(uint8_t source, cJSON *params)
{
    (void)params;
    if (s_pt1000_read_temp) {
        float temp = s_pt1000_read_temp();
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"temp_c\":%.2f,\"status\":\"ok\"}", temp);
        send_response(source, resp);
    } else {
        send_response(source, "{\"temp_c\":0,\"status\":\"error\",\"msg\":\"PT1000 not available\"}");
    }
}

static void cmd_calibrate(uint8_t source, cJSON *params)
{
    int ref_ohm = 1000;
    cJSON *ref_item = cJSON_GetObjectItem(params, "ref_ohm");
    if (ref_item && cJSON_IsNumber(ref_item)) {
        ref_ohm = ref_item->valueint;
    }

    if (s_pt1000_calibrate) {
        esp_err_t err = s_pt1000_calibrate(ref_ohm);
        if (err == ESP_OK) {
            send_response(source, "{\"status\":\"ok\",\"msg\":\"calibration done\"}");
        } else {
            send_response(source, "{\"status\":\"error\",\"msg\":\"calibration failed\"}");
        }
    } else {
        send_response(source, "{\"status\":\"error\",\"msg\":\"PT1000 not available\"}");
    }
}

static void cmd_get_status(uint8_t source, cJSON *params)
{
    (void)params;
    dtu_config_t cfg;
    dtu_config_get(&cfg);

    char resp[320];
    snprintf(resp, sizeof(resp),
             "{\"wifi_mode\":%d,\"uart1_mode\":%d,\"uart2_mode\":%d,"
             "\"socket1\":\"%s:%u\",\"socket2\":\"%s:%u\"}",
             cfg.wifi_mode, cfg.uart1_mode, cfg.uart2_mode,
             cfg.srv1_ip, cfg.srv1_port,
             cfg.srv2_ip, cfg.srv2_port);
    send_response(source, resp);
}

static void cmd_blackbox(uint8_t source, cJSON *params)
{
    int count = 20;
    cJSON *count_item = cJSON_GetObjectItem(params, "count");
    if (count_item && cJSON_IsNumber(count_item)) {
        count = count_item->valueint;
        if (count > 100) count = 100;
    }

    blackbox_entry_t *entries = malloc(count * sizeof(blackbox_entry_t));
    if (!entries) {
        send_response(source, "{\"status\":\"error\",\"msg\":\"no memory\"}");
        return;
    }

    int n = blackbox_read_recent(entries, count);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char buf[128];
        blackbox_format_entry(&entries[i], buf, sizeof(buf));
        cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
    }
    cJSON_AddItemToObject(root, "entries", arr);
    cJSON_AddNumberToObject(root, "count", n);

    char *resp = cJSON_PrintUnformatted(root);
    send_response(source, resp);
    cJSON_free(resp);
    cJSON_Delete(root);
    free(entries);
}

static void cmd_log_ctrl(uint8_t source, cJSON *params)
{
    cJSON *mod_item = cJSON_GetObjectItem(params, "module");
    cJSON *en_item = cJSON_GetObjectItem(params, "enable");

    if (!mod_item || !en_item) {
        send_response(source, "{\"status\":\"error\",\"msg\":\"missing params\"}");
        return;
    }

    if (cJSON_IsTrue(en_item)) {
        dtu_log_enable_all(true);
    } else if (cJSON_IsFalse(en_item)) {
        dtu_log_enable_all(false);
    }

    send_response(source, "{\"status\":\"ok\"}");
}

static void cmd_ota(uint8_t source, cJSON *params)
{
    cJSON *url_item = cJSON_GetObjectItem(params, "url");
    if (!url_item || !cJSON_IsString(url_item)) {
        send_response(source, "{\"status\":\"error\",\"msg\":\"missing url\"}");
        return;
    }

    if (s_ota_start) {
        s_ota_start(url_item->valuestring);
        send_response(source, "{\"status\":\"started\"}");
    } else {
        send_response(source, "{\"status\":\"error\",\"msg\":\"OTA not available\"}");
    }
}

static void cmd_restart(uint8_t source, cJSON *params)
{
    (void)params;
    send_response(source, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void process_command(uint8_t source, const uint8_t *data, uint16_t len)
{
    /* 命令帧格式为：+++DTU_CMD:{json}+++，这里只截取 JSON 部分。 */
    const char *json_start = (const char *)data + CMD_PREFIX_LEN;
    uint16_t json_len = len - CMD_PREFIX_LEN - CMD_SUFFIX_LEN;

    char *json_str = malloc(json_len + 1);
    if (!json_str) return;
    memcpy(json_str, json_start, json_len);
    json_str[json_len] = '\0';

    DTU_LOGI(DTU_LOG_MOD_CMD, TAG, "cmd: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        send_response(source, "{\"status\":\"error\",\"msg\":\"invalid JSON\"}");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) {
        send_response(source, "{\"status\":\"error\",\"msg\":\"missing cmd\"}");
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd->valuestring;

    /* 当前命令集以维护和诊断为主。 */
    if (strcmp(cmd_str, "read_temp") == 0) {
        cmd_read_temp(source, root);
    } else if (strcmp(cmd_str, "calibrate") == 0) {
        cmd_calibrate(source, root);
    } else if (strcmp(cmd_str, "status") == 0) {
        cmd_get_status(source, root);
    } else if (strcmp(cmd_str, "blackbox") == 0) {
        cmd_blackbox(source, root);
    } else if (strcmp(cmd_str, "log_ctrl") == 0) {
        cmd_log_ctrl(source, root);
    } else if (strcmp(cmd_str, "ota") == 0) {
        cmd_ota(source, root);
    } else if (strcmp(cmd_str, "restart") == 0) {
        cmd_restart(source, root);
    } else {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"msg\":\"unknown cmd: %s\"}", cmd_str);
        send_response(source, resp);
    }

    cJSON_Delete(root);
}

static void cmd_proc_task(void *arg)
{
    (void)arg;
    cmd_request_t req;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &req, portMAX_DELAY)) {
            process_command(req.source, req.data, req.len);
        }
    }
}

esp_err_t cmd_handler_init(void)
{
    /* 命令流量一般不大，用固定深度队列即可满足维护场景。 */
    s_cmd_queue = xQueueCreate(8, sizeof(cmd_request_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    xTaskCreate(cmd_proc_task, "cmd_proc", 4096, NULL, 7, &s_cmd_task_handle);
    DTU_LOGI(DTU_LOG_MOD_CMD, TAG, "cmd_handler init done");
    return ESP_OK;
}

bool cmd_handler_is_command(const uint8_t *data, uint16_t len)
{
    if (len < CMD_PREFIX_LEN + CMD_SUFFIX_LEN) return false;
    if (memcmp(data, CMD_PREFIX, CMD_PREFIX_LEN) != 0) return false;
    if (memcmp(data + len - CMD_SUFFIX_LEN, CMD_SUFFIX, CMD_SUFFIX_LEN) != 0) return false;
    return true;
}

esp_err_t cmd_handler_submit(uint8_t source, const uint8_t *data, uint16_t len)
{
    cmd_request_t req = { .source = source };
    uint16_t copy_len = (len > sizeof(req.data)) ? sizeof(req.data) : len;
    memcpy(req.data, data, copy_len);
    req.len = copy_len;

    if (xQueueSend(s_cmd_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void cmd_handler_set_response_cb(cmd_response_cb_t cb)
{
    s_response_cb = cb;
}
