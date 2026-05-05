# ESP32C3_DTU_double_uart

基于 ESP32-C3 和 ESP-IDF v5.4.2 的双串口 RS485 DTU 固件工程，支持双路透明传输、Web 配置、BLE 维护、PT1000 温度采集、黑匣子事件记录以及 OTA 在线升级。

## 功能概览

- 双路 RS485 半双工接入
- TCP / UDP / MQTT / HTTP 透明传输
- Web 本地配置与状态查看
- BLE 轻量配置与维护入口
- PT1000 温度采集与校准
- 黑匣子事件记录
- 基于 `esp_https_ota()` 的 URL OTA 升级

## 软件架构

工程采用 `main + components` 的组织方式：

- `main/`
  系统启动入口，负责各组件初始化和整体编排。
- `components/dtu_config/`
  全局配置管理与 NVS 持久化。
- `components/dtu_log/`
  模块化日志开关与日志封装。
- `components/blackbox/`
  黑匣子事件记录与读取。
- `components/wifi_mgr/`
  WiFi STA / AP / APSTA 管理。
- `components/rs485_uart/`
  双路 RS485 串口驱动。
- `components/net_transparent/`
  串口与 TCP / UDP / MQTT / HTTP 的桥接与透明传输。
- `components/cmd_handler/`
  `+++DTU_CMD:json+++` 命令解析与执行。
- `components/pt1000_adc/`
  PT1000 温度采集与校准。
- `components/web_server/`
  Web 页面与 REST API。
- `components/ble_service/`
  BLE 配置、命令和运维入口。
- `components/ota_update/`
  OTA 升级管理。
- `docs/`
  软件设计方案、使用手册和接口附录。

## 硬件接口

### RS485-A

- UART: `UART1`
- TX: `GPIO10`
- RX: `GPIO5`
- DE/RE: `GPIO6`

### RS485-B

- UART: `UART0`
- TX: `GPIO21`
- RX: `GPIO20`
- DE/RE: `GPIO4`

## 默认配置摘要

当前代码中的默认配置位于 `components/dtu_config/dtu_config.c`：

- `wifi_mode = STA`
- `srv1_ip = 192.168.1.100`
- `srv2_ip = 192.168.1.100`
- `srv1_port = 8080`
- `srv2_port = 8080`
- `uart1_mode = TCP`
- `uart2_mode = TCP`
- `uart1_baud = 9600`
- `uart2_baud = 9600`
- `mqtt_client_id = esp32c3_dtu`

## 编译环境

- ESP-IDF: `v5.4.2`
- Target: `esp32c3`

## 编译

```powershell
cmd /c "call E:\esp-idf\esp-idf_v5.4.2\Espressif\frameworks\esp-idf-v5.4.2\export.bat && python E:\esp-idf\esp-idf_v5.4.2\Espressif\frameworks\esp-idf-v5.4.2\tools\idf.py -DIDF_TARGET=esp32c3 build"
```

生成固件：

```text
build/ESP32C3_DTU.bin
```

## 烧录

将 `COMx` 替换为实际串口号：

```powershell
cmd /c "call E:\esp-idf\esp-idf_v5.4.2\Espressif\frameworks\esp-idf-v5.4.2\export.bat && python E:\esp-idf\esp-idf_v5.4.2\Espressif\frameworks\esp-idf-v5.4.2\tools\idf.py -p COMx -DIDF_TARGET=esp32c3 flash monitor"
```

## OTA 说明

工程使用标准双 OTA 分区方案：

- `otadata`
- `ota_0`
- `ota_1`

OTA 由 `components/ota_update/ota_update.c` 实现，升级流程如下：

1. 通过 Web API 或命令接口传入固件 URL
2. 调用 `ota_update_start(url)`
3. 内部使用 `esp_https_ota()` 下载并写入空闲 OTA 分区
4. 下载成功后自动重启
5. 新固件启动后在 `ota_update_init()` 中确认镜像有效

当前支持的 Web OTA 接口：

```text
POST /api/v1/ota
```

请求体示例：

```json
{
  "url": "http://192.168.1.50:8080/ESP32C3_DTU.bin"
}
```

## Web API 摘要

- `GET /`
- `GET /api/v1/config`
- `PUT /api/v1/config`
- `POST /api/v1/config/apply`
- `GET /api/v1/status`
- `GET /api/v1/status/temperature`
- `POST /api/v1/calibrate`
- `GET /api/v1/blackbox`
- `DELETE /api/v1/blackbox`
- `GET /api/v1/log_config`
- `PUT /api/v1/log_config`
- `POST /api/v1/ota`

## 文档

详细资料见 `docs/`：

- `docs/软件设计方案.md`
- `docs/使用手册.md`
- `docs/交付附录-系统图与接口表.md`
