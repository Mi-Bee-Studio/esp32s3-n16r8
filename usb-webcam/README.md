# MiBee usb-webcam — ESP32-S3-N16R8 板 UVC 摄像头固件

把 GOOUUU N16R8 相机板变成**即插即用的 USB 摄像头**：插 PC 原生 USB 口即枚举为标准 UVC 设备，Windows/OBS/浏览器/会议软件直接认，免驱动。

**缘起（2026-10-08）**：台面单元（MAC `be:5c`）WiFi 射频经隔离测试定性硬件嫌疑（cam/AGENTS.md），但 USB/摄像头/串口全正常——网络相机干不了，改派 PC 摄像头。

## 硬件接法

- **原生 USB 口**（GPIO19/20，板上另一个 USB-C）→ PC：UVC 视频流
- **CH343 口** → PC（可同插）：UART0 控制台日志 115200（serialtap 设备名 `ch343`）
- 两口同时插 = 视频 + 日志全可观测

## 规格

| 项 | 值 |
|---|---|
| 传感器 | OV5640（OV3660 自动识别） |
| 输出 | MJPEG 640×480 @15fps（USB Full-Speed 实际 ~10-15fps） |
| USB 传输 | Bulk（esp-iot-solution S3 CI 同款） |
| 分辨率切换 | 宿主可选 QVGA/HVGA/VGA/SVGA（UVC 协商，传感器全量重配） |
| 供电 | USB 总线供电（WiFi 关闭，功耗远低于 cam 固件） |

## 构建 / 烧录

```bash
# ESP-IDF v6.0.1（组件要求 idf ≥5.0 均可）；首次构建组件管理器自动下载依赖
cd usb-webcam && idf.py set-target esp32s3 && idf.py build

# 烧录（serialtap，经 CH343 口；bin 绝对路径）
serialtap flash ch343 \
  '<abs>/build/bootloader/bootloader.bin'@0x0 \
  '<abs>/build/partition_table/partition-table.bin'@0x8000 \
  '<abs>/build/mibee_usb_webcam.bin'@0x10000 \
  --esptool <esptool.exe> --chip esp32s3 --baud 460800
```

- 分区表 `nvs`/`phy_init` 偏移与 cam/ 一致：**覆盖刷本固件不动 cam 的 NVS**，两固件可随时互换重刷（cam 恢复后配置原样）。
- 烧完 CH343 口按 RTS 复位，原生 USB 口重插一次即枚举 `MiBee USB Webcam (N16R8)`。

## 已知限制 / Roadmap

- **无 OTA**（已登记缺口）：本用途的目标单元 WiFi 已坏（硬件），无升级通道；恢复手段 = CH343/原生口重刷（serialtap 正道）。若日后给 WiFi 正常的板用本固件，按家族 `/api/ota` 模式补双 OTA 槽。
- 无麦克风（板无 MIC 硬件）；USB-FS 带宽上限 ~1MB/s，VGA 以上分辨率帧率线性下降。
- 画面方向：`menuconfig → MiBee USB Webcam Configuration` 里 VFLIP/HMIRROR 常驻配置。

## 移植来源与许可

- [espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution) `examples/usb/device/usb_webcam`（Apache-2.0）——回调骨架
- 组件：`espressif/usb_device_uvc`（UVC 设备类）、`espressif/esp32-camera`（与 cam/ 同版本线）
