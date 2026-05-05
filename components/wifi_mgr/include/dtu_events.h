#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 系统级事件位定义，供各组件通过全局事件组做轻量同步。 */
#define EVT_WIFI_CONNECTED      (1 << 0)
#define EVT_WIFI_DISCONNECTED   (1 << 1)
#define EVT_NET_TCP_CONNECTED   (1 << 2)
#define EVT_NET_UDP_READY       (1 << 3)
#define EVT_NET_MQTT_CONNECTED  (1 << 4)
#define EVT_CONFIG_LOADED       (1 << 5)
#define EVT_BLE_CONNECTED       (1 << 6)

/* 事件组实例在 main.c 中创建，这里仅做跨模块声明。 */
extern EventGroupHandle_t g_dtu_event_group;

#ifdef __cplusplus
}
#endif
