# AGENTS.md — esp32s3-n16r8（主板仓导航）

> 2026-10-08 由 `esp32s3-n16r8-cam` 改造为主板仓（板级文档标准 + 主板为根规范）。
> 本文件只做导航与跨项目契约；**cam/ 子项目的详细 AGENTS.md（任务布局、家族契约、PIT 坑）优先于本文件**。

## 子项目地图

| 目录 | 是什么 | 工具链 | 备注 |
|---|---|---|---|
| `cam/` | MiBee Cam 网络相机固件（原仓整体迁入，git 历史保留） | ESP-IDF **v6.0.1（pin 死）** | 内部 `cam/AGENTS.md` 为准；AT/文档四仓 md5 契约文件在 `cam/docs/at-command.md`、`cam/main/at_command.c` |
| `usb-webcam/` | UVC USB 摄像头固件（esp32-camera + espressif/usb_device_uvc） | ESP-IDF v6.0.1（组件要求 idf ≥5.0） | 上板验证记录见项目 README |

## 跨项目契约

- **硬件事实源**：板级引脚/PSRAM/分区见根 README（Board Overview / Pinout Diagram 两节，板级文档标准）；cam 项目内细节以其 AGENTS.md 与 `cam/docs/hardware.md` 为准，冲突时以更近实测者为准并回改另一处。
- **引脚不跨项目共享配置**：cam 的引脚在 `cam/sdkconfig.defaults`（Kconfig），usb-webcam 的引脚在其 `main/camera_pin.h`（同表数据、两份载体——主板为根"不共享代码"规范的代价，改引脚两处同步）。
- **烧录纪律**：优先 serialtap（设备名 `ch343`；原生口未接时唯一通道）。usb-webcam 烧录**只写 bootloader/分区表/factory**（其分区表 nvs 偏移与 cam 一致 → 覆盖刷不动 cam 的 NVS 配置，两固件可互换重刷）。
- **双远端**：origin=GitHub `Mi-Bee-Studio/esp32s3-n16r8`（旧名 301 重定向）、gitea=内网 Gitea 镜像 `home-lab/esp32s3-n16r8`（内网地址不入公开仓，见工作区根 AGENTS.md）。main 受保护，改动走 PR。

## 台面单元（MAC be:5c）现状

WiFi 射频硬件嫌疑（2026-10-08 隔离测试定性，全过程见 cam/AGENTS.md 末两节）——USB/摄像头/串口全正常。**该单元现跑 usb-webcam 固件，作 PC 摄像头使用**；cam 固件随时可重刷回去（NVS 配置保留）。
