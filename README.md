# ESP32-S3-N16R8 相机板（GOOUUU）

> 主板仓（board-as-root）：本 README 只写板子；每个项目一个子目录，独立可编译。
> 2026-10-08 由 `esp32s3-n16r8-cam` 仓改造而来（GitHub/Gitea 仓已同步改名，旧地址自动重定向）。

```
esp32s3-n16r8/
├── README.md        # 本文件：板子硬件事实
├── cam/             # MiBee Cam 网络相机固件（原仓整体迁入，自带 AGENTS.md）
└── usb-webcam/      # USB 摄像头固件（UVC，插 PC 即普通 webcam）
```

- 每个子项目自带完整构建三件套（CMakeLists.txt / main/ / sdkconfig.defaults）：`cd <项目> && idf.py build`。
- 项目间不共享代码，共性先拷贝（主板为根规范）。
- **在 cam/ 里工作时以其内部 AGENTS.md 为准**（任务布局、引脚、坑、家族契约）。

## Board Overview

| Item | Value | Notes |
|------|-------|-------|
| 板卡 | GOOUUU ESP32-S3-N16R8 相机板 | 厂商由引脚表推断（cam/AGENTS.md） |
| 模组 | ESP32-S3-WROOM-1 **N16R8** | 16 MB Quad Flash · 8 MB **Octal** PSRAM |
| SoC | ESP32-S3 (Xtensa LX7 双核 @ 240 MHz) | USB-OTG（GPIO19/20）+ USB-Serial/JTAG 双外设 |
| 摄像头 | **OV5640 实载**（设计兼容 OV3660） | 传感器由驱动自适应识别；DVP 并口 |
| USB ×2 | ① 原生 USB 口（GPIO19/20，JTAG/OTG）② CH343 USB-UART（UART0 控制台/烧录） | 两口同时可用：UVC 走原生口、日志走 CH343 |
| Flash LED | GPIO 2 / 3 / 46（开机探测） | cam 固件暴露 `/api/led` |
| SD / 麦克风 | 无 | 硬件豁免项 |
| 已知硬件问题 | 本台台面单元（MAC `80:b5:4e:c2:be:5c`）**WiFi 射频路径疑似损坏**（2026-10-08 隔离测试定性，见 cam/AGENTS.md）——USB/摄像头/串口均正常 | 该单元改派 USB 摄像头用途（usb-webcam/） |

## Pinout Diagram（功能引脚图，USB 口朝上/元件面视角）

```
        ┌────────────────────────┐
        │   [原生 USB]   [CH343  │   ① 原生 USB：GPIO19(D-)/GPIO20(D+)
        │    GPIO19/20    USB]   │      UVC 固件走此口；亦作 USB-JTAG 烧录
        │                        │   ② CH343 USB-UART → UART0：AT 控制台 + 烧录
        │   ESP32-S3-WROOM-1     │      （cam 与 usb-webcam 的日志都从这里出）
        │      N16R8             │
        │                        │      摄像头连接器（DVP，实测引脚表）：
        │   [摄像头连接器]        │      XCLK=15  SIOD=4  SIOC=5  PCLK=13
        │    OV5640/OV3660       │      D0=11 D1=9  D2=8  D3=10
        │                        │      D4=12 D5=18 D6=17 D7=16
        │   Flash LED: 2/3/46    │      VSYNC=6 HREF=7  PWDN/RESET=NC
        └────────────────────────┘
```

> 引脚出处：cam 固件 `cam/sdkconfig.defaults`（上板验证过）；物理板边排布未在仓内文档化，此处画功能引脚图不臆造物理位置。

## Projects

| Project | 是什么 | 构建工具链 | 看门狗 | Web/API OTA |
|---------|--------|-----------|--------|-------------|
| [`cam/`](cam/) | MiBee Cam 网络相机（MJPEG/RTSP/ONVIF/AI 检测/Web UI，家族四仓契约成员） | ESP-IDF **v6.0.1（pin 死）** | ✅ TWDT | ✅ `/api/ota` |
| [`usb-webcam/`](usb-webcam/) | USB 摄像头（UVC over 原生 USB，插 PC 即普通 webcam，无需驱动） | ESP-IDF v6.0.1（idf ≥5.0 均可） | ✅ TWDT | ❌ 已知缺口（roadmap：见项目 README；本台台面单元 WiFi 已坏，OTA 无通道） |

固件基线规范（全家桶强制：看门狗必开、硬件允许必须 OTA）——usb-webcam 的 OTA 属已登记缺口，硬件豁免情形见项目 README。

## Flash / Debug

- 两口都可烧录：CH343（UART0，serialtap 设备名 `ch343`）或原生口（USB-Serial-JTAG，复位即回枚举，与 UVC 固件共存无冲突）。
- 日志：两项目控制台都走 UART0@115200（CH343 口），serialtap 常驻采集。
