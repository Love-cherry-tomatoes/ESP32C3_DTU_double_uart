#include "ble_service.h"
#include "esp_gatts_api.h"
#include "esp_log.h"

static const char *TAG = "BLE_GATT";

/* 
 * 当前工程没有使用静态 GATT 属性表，而是在 ble_service.c 的事件回调里动态创建服务。
 * 这个文件主要作为设计说明保留，提醒维护者：
 * 1. UUID 与索引定义集中在 ble_service.c；
 * 2. 若后续切换为静态属性表实现，可从这里继续扩展；
 * 3. 目前属性索引 enum 也在 ble_service.c 中维护。
 */
