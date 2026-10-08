# AGENTS.md — ESP32-S3-N16R8 CAM

> Firmware project for an ESP32-S3-N16R8 module + OV3660 camera. **Production-ready** with MJPEG streaming, AI detection, RTSP, ONVIF, and responsive web UI.

## AT command interface (family contract v1.0, 2026-09-04)

统一契约：`docs/at-command.md`（四仓 md5 一致，地位同 api-contract）。核心集：
`AT / AT+HELP / AT+GMR / AT+STATUS / AT+WIFI?|= / AT+IP? / AT+CAMRES?|= / AT+CAMQUAL?|= /
AT+REBOOT / AT+RESTORE`（+能力裁剪项）。红线：**任何读指令不回显密码**；CAMQUAL 边界
10-63（PIT-021）。本板串口 /dev/ttyUSB1（CH340）。CAMRES/CAMQUAL 走 camera_reinit 热重配（AI 全开时锁 VGA）；实现于 main/at_command.c（含 AI/LED/RTSPPASS 扩展）。

## Hardware target

| Item | Value | Notes |
|------|-------|-------|
| Module | ESP32-S3-WROOM-1 **N16R8** | N16 = 16 MB Quad Flash · R8 = 8 MB **Octal** PSRAM |
| SoC | ESP32-S3 (Xtensa LX7 dual-core @ 240 MHz) | USB-OTG + USB-Serial/JTAG |
| Camera | **OV3660** (3 MP, 1/5", max QXGA 2048×1536) | NOT the OV2640 from the reference repos — see below |
| USB | USB-Serial/JTAG (enumerates as `/dev/ttyACM0`) | |

### Why N16R8 changes the design vs the reference repos

- **16 MB Flash** (vs 8 MB on `seeed-esp32s3-cam`, 4 MB on `ai-thinker-esp32-cam`) → partition table can hold two **larger** OTA slots, bigger SPIFFS/NVS, or a factory image. Re-plan `partitions.csv` from scratch; do **not** copy the 8 MB layout.
- **8 MB Octal PSRAM** → same module family as the XIAO board, so the S3-Octal gotchas carry over (see *Octal PSRAM* below).
- **OV3660 ≠ OV2640**:
  - Higher max resolution (QXGA). Defaults copied from OV2640 (typically UXGA) will misallocate frame buffers.
  - esp32-camera exposes it via the same `esp_camera_*` API; `PIXFORMAT_JPEG` still works, but JPEG engine quality/rate differs.
  - Sensor ID is `0x77` (OV2640 is `0x26`/`0x42`) — useful for `esp_camera_sensor_get()` sanity checks.

## Toolchain

| Tool | Version | Path / Notes |
|------|---------|--------------|
| ESP-IDF | **v6.0.1 (pinned)** | `~/.espressif/v6.0.1/esp-idf/` |
| Component: `espressif/esp32-camera` | `^2.1.6` | Proven across both reference repos, supports OV3660 |
| Target | `esp32s3` | Set once per build dir |

Activation (every new shell):
```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
```

## Reference repos (carry conventions forward, do NOT copy pin tables)

| Repo | What to steal | What NOT to copy |
|------|---------------|------------------|
| https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam | S3 + Octal PSRAM sdkconfig patterns, `main/` flat module layout, dual-OTA partitioning, SPIFFS web UI embedding, build/flash/release workflow | XIAO ESP32-S3 Sense **camera pin map** (different board) |
| https://github.com/Mi-Bee-Studio/ai-thinker-esp32-cam | Simpler motion-detect/MJPEG core, ESP-IDF v6.0.1 CI badge convention | Everything ESP32-specific (plain ESP32 has no Octal PSRAM, DMA differs, IRAM pressure tuning is ESP32-only) |

The `seeed-esp32s3-cam` repo has a detailed `AGENTS.md` worth reading for S3 patterns; this file is its sibling, scoped to N16R8 + OV3660.

## Shipped Features

The firmware is production-ready with the following modules and features:

### Core Modules (15 modules)

| Module | Files | Purpose |
|--------|-------|---------|
| main.c | main.c | App entry, boot sequence orchestrator |
| config_manager | config_manager.c/h | NVS-backed config, 16 keys, TYPE_U8/TYPE_I8 |
| camera_driver | camera_driver.c/h | OV3660 init, sensor settings, coordinated reinit |
| frame_broadcaster | frame_broadcaster.c/h | Frame grab task on Core 1, publisher-subscriber pattern |
| mjpeg_streamer | mjpeg_streamer.c/h | HTTP MJPEG streaming via chunked multipart |
| ai_pipeline | ai_pipeline.cpp/h | Face + motion + QR detection, 640×480 buffers, ESP-DL |
| web_server | web_server.c/h | REST API (11 endpoints), SPIFFS static files |
| web_ui | index.html, style.css, app.js, i18n.js | Browser UI, zh/en i18n, light/dark theme |
| wifi_manager | wifi_manager.c/h | WiFi STA mode connection |
| flash_led | flash_led.c/h | GPIO flash LED control |
| at_command | at_command.c/h | Serial AT command interface |
| rtsp_server | rtsp_server.cpp/h | RTSP server with MJPEG-only streaming |
| onvif_service | onvif_service.c/h | ONVIF SOAP service |
| onvif_discovery | onvif_discovery.c/h | ONVIF WS-Discovery protocol |
| status_led | status_led.c/h | GPIO status LED |

### REST API Endpoints

> **2026-09-02 契约 v1.0 统一化**（权威规范：`docs/api-contract.md`，下表已过时）：
> 新增核心端点 `GET /api/capture`、`GET /api/scan`、`POST /api/reset`、`POST /api/reboot`、
> `POST /api/time`、`GET /api/auth`、`GET /metrics`、`GET /api/led`；
> `POST /api/camera` framesize 合法域收窄为 0-15（与广播的 `supported_resolutions` 一致）；
> capabilities 增加 `api_version`/`wifi_scan`；status 字段对齐契约
> （`camera_resolution`→`resolution`、`mjpeg_clients`→`stream_clients`，新增 `camera`/`device_name`/
> `firmware_version`/`wifi_state`/`min_heap`/`stream_clients_max`）；config 新增 `device_name` 键。
> **RTSP 鉴权已启用**：vendored `components/espp__rtsp`（自 seeed 复制，含 digest 补丁）+
> 直接依赖 `espp/base_component|socket|task`（idf_component.yml 已改，勿再加 espp/rtsp）。
> ONVIF GetSnapshotUri 已指向 `:80/api/capture`。
>
> **契约 v1.1（2026-09-02）**：移植 seeed `ota_updater`（`/api/ota`、`/api/ota/info`、
> `/api/ota/upload`、`/api/ota/spiffs`），`ota:true`；统一公开默认密码 `mibeecam2026`（Kconfig 默认值，可入文档；本地可在 gitignored sdkconfig 覆盖）、拒绝 <6 位密码；
> 修复首次设密未持久化 bug（`known_keys` 白名单曾漏 `web_password`）；api_version=1.1。

All business endpoints use the `/api/` prefix. Returns JSON envelope `{"ok":true,"data":...}` on success, `{"ok":false,"error":"..."}` on failure.

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | open | Device status (WiFi, camera, AI, system) |
| GET | `/api/config` | open | Current configuration (passwords masked) |
| POST | `/api/config` | write | Partial config update (WiFi triggers reboot) |
| GET | `/api/camera` | open | Camera configuration |
| POST | `/api/camera` | write | Update camera settings (framesize/quality requires reinit) |
| GET | `/api/capabilities` | open | Board capability flags (12 booleans) |
| GET | `/api/scan` | open | WiFi AP scan |
| POST | `/api/ai` | write | Toggle AI features (live + persist) |
| GET | `/api/ai/status` | open | AI detection results (face boxes, motion, QR) |
| POST | `/api/led` | write | Flash LED brightness control |
| GET | `/api/led` | open | Flash LED state |
| OPTIONS | `/*` | — | CORS preflight (204 No Content) |

**Auth:** `X-Password` header for write operations. When `web_password` is empty (first boot), all writes return 401 `SET_PASSWORD_FIRST` except `POST /api/config` with a `web_password` field (first-time setup).

**MJPEG stream:** Separate TCP server on port `:81` (independent of main web server on port 80).

**Exempt paths:** `/onvif/*` (SOAP) are not under `/api/`.

### Web UI Features

> **2026-09-02 UI v3 "Honey"**（与 seeed/luatos md5 一致的单一源）：蜂蜜琥珀主题、暗色优先、
> 视频主舞台 + 玻璃控制条 + 指标 chips + 分段式控制台、鉴权抽屉（401 自动唤起并重试）、
> 模态确认、按钮忙态、断流骨架屏。设计令牌集中在 style.css 顶部，规范以 seeed 仓为准。
> 下方旧描述的组件清单已部分过时。

Single-page application served from SPIFFS. Four files:
- `index.html` — page structure
- `app.js` — logic and API calls
- `style.css` — light/dark theme styles
- `i18n.js` — zh/en bilingual translations (auto-detect, persisted in localStorage)

Controls are shown or hidden based on `GET /api/capabilities`. The SPA baseline originates from this board and is ported to all boards.

- Settings panels: Camera, AI, Flash LED, Network, Streaming, System
- Real-time AI overlay (face boxes, motion score, QR text)
- Responsive design (mobile 480px → desktop 1280px+)
- Stream reconnect with exponential backoff (1s → 30s)

### Capabilities

This board returns the following from `GET /api/capabilities`:

| Capability | Supported |
|------------|-----------|
| ai | ✅ |
| sd | ❌ |
| audio | ❌ |
| ota | ✅（v1.1 起，eb65387 移植 ota_updater，Web OTA 分区翻转自测通过） |
| mic | ❌ |
| flash_led | ✅ |
| recording | ❌ |
| timelapse | ❌ |
| onvif | ✅ |
| rtsp | ✅ |
| websocket | ❌ |
| mdns | ✅ |

### AI Features

- **Face detection** (ESP-DL HumanFaceDetect MSRMNP_S8_V1)
- **Motion detection** (frame-difference on grayscale)
- **QR decode** (quirc library)
- Requires VGA resolution (640×480)
- Live toggle via web UI or REST API
- Mutex-protected results for thread-safe polling

### Streaming

- **MJPEG** via HTTP (`/stream`, multiple clients)
- **RTSP** server (MJPEG-only, digest auth)
- **ONVIF** discovery (WS-Discovery) + SOAP service

### Configuration

- NVS namespace: "mibee_cfg"（家族配置契约 v1.0，`docs/config-contract.md`；
  逐键 + `schema_ver=1`，键名 ≤15 字符构建期断言，单键写失败只 WARN 不中止批次）
- 26 keys（含 schema_ver）。Supported keys: wifi_ssid, wifi_pass, wifi_ssid_2,
  wifi_pass_2, cam_framesize, cam_fps, cam_quality, xclk_freq_mhz,
  ai_face_en, ai_motion_en, ai_qr_en（2026-09-05 契约对齐改名，旧键
  ai_face_enable/ai_qr_enable 惰性迁移）, rtsp_user, rtsp_pass, onvif_enable,
  cam_brightness, cam_contrast, cam_saturation, cam_sharpness, cam_hmirror,
  cam_vflip, device_name, timezone, ap_fallback（JSON 名 allow_ap_fallback）
- Type support: TYPE_STRING / TYPE_U8 / TYPE_I8
- GET/POST /api/config 字段名 = 契约 JSON 名；POST 校验矩阵 = 契约 §4
  （cam_quality 10-63、cam_fps 1-30、cam_framesize ∈ supported_resolutions、
  xclk ∈{10,16,20}、web_password ≥6、timezone 1-47、字符串先验长度拒绝）

## Octal PSRAM (non-negotiable on this module)

Copied verbatim from the working `seeed-esp32s3-cam` config — verified necessary on S3 + 8 MB Octal:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y            # Octal, NOT QUAD — R8 module
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
# 64B cache line is MANDATORY for Octal DDR mode. 32B causes silent data corruption.
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

Frame buffers for OV3660 at any meaningful resolution **must** live in PSRAM (`CAMERA_FB_IN_PSRAM`); internal DRAM is far too small.

## Project layout

```
./
├── main/                 # Flat C module layout (one .c/.h pair per subsystem)
│   ├── main.c            # app_main() entry
│   ├── camera_driver.*   # OV3660 init wrapper around esp_camera
│   ├── config_manager.*  # NVS-backed config
│   ├── frame_broadcaster.*  # Frame grab + publisher
│   ├── mjpeg_streamer.*     # HTTP MJPEG streaming
│   ├── ai_pipeline.*        # Face/motion/QR detection (C++)
│   ├── web_server.*         # REST API
│   ├── web_ui/              # HTML/JS/CSS for browser UI
│   │   ├── index.html
│   │   ├── style.css
│   │   ├── app.js
│   │   └── i18n.js
│   ├── wifi_manager.*       # WiFi management
│   ├── flash_led.*          # Flash LED control
│   ├── at_command.*         # Serial AT commands
│   ├── rtsp_server.*        # RTSP server (C++)
│   ├── onvif_service.*      # ONVIF SOAP service
│   ├── onvif_discovery.*    # ONVIF WS-Discovery
│   ├── status_led.*         # Status LED
│   ├── CMakeLists.txt       # Component build
│   └── idf_component.yml    # Component dependencies
├── docs/                  # Documentation
│   ├── architecture.md    # Module map, boot sequence, data flow
│   ├── hardware.md        # Pin map, PSRAM constraints, partitions
│   ├── web-api.md         # REST endpoint reference
│   ├── web-ui.md          # UI features, i18n, theme
│   └── development.md     # Build, flash, CI, contributing
├── partitions.csv         # Custom partition table (16 MB Flash)
├── sdkconfig.defaults     # Hardware pin map + PSRAM + watchdog + lwIP
├── CMakeLists.txt         # project() + spiffs_create_partition_image()
├── AGENTS.md              # This file
├── README.md              # Project README
└── .github/workflows/     # Tag-triggered release CI
    └── release.yml
```

Conventions:
- `sdkconfig.defaults` is the **single source of truth** for hardware pin config. Do not put pin numbers in `Kconfig.projbuild` menu items — both reference repos keep them in `sdkconfig.defaults` and that's where operators look.
- Project name in `CMakeLists.txt`: `mibee_cam` (consistent MiBee Cam branding across the family).
- `sdkconfig` (the generated one) and `managed_components/` are gitignored — only `sdkconfig.defaults` is committed.

## Build / flash

```bash
# First time in a fresh shell
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3

# Build
idf.py build

# Flash full image  — NOTE flashing policy (root AGENTS.md, 2026-09-04):
# Web OTA is the DEFAULT delivery path since eb65387 (2026-09-05, ota:true 已上板
# 验证：/api/ota/upload 流式上传 + ota_0/ota_1 翻转)。USB 是回退手段。
idf.py -p /dev/ttyUSB1 flash

# App-only fast iteration (offset 0x10000 is ota_0)
esptool --chip esp32s3 -p /dev/ttyUSB1 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 build/mibee_cam.bin

# Clean rebuild
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
```

- **Serial port**: 本板实际走 CH340（USB 转 UART），枚举为 `/dev/ttyUSB1`（非 ACM0 —
  早期文档写 USB-Serial/JTAG 有误）。CH340 open-reset 陷阱适用：别裸开端口观察，
  用 `tools/overnight_log.py` 持有。Confirm with `ls /dev/serial/by-id/`.
- **Baudrate**: 115200 (firmware default).
- **Permission**: user must be in `uucp` (Arch) or `dialout` (Debian/Ubuntu).
- **led_strip patch**: `patches/espressif__led_strip/` fixes a compile error (led_strip 2.5.5
  uses `MALLOC_CAP_*` without including `esp_heap_caps.h` under IDF v6.0.1). The root
  `CMakeLists.txt` copies it over `managed_components/` at configure time — source-only,
  so fresh clones and CI get it automatically. Never edit files inside `managed_components/`
  directly; edit the copy under `patches/`. If `idf.py fullclean` ever errors with a
  managed-components hash mismatch, `rm -rf managed_components build` and rebuild.

## Verified Hardware Attributes

### Board

- **Vendor**: GOOUUU (verified from pin map)
- **Model**: ESP32-S3-N16R8 + OV3660 camera board
- **Pin map**: GOOUUU-specific (NOT XIAO or other vendors)

### OV3660 Camera

- **Sensor ID**: 0x77 (verified via SCCB read)
- **Interface**: SCCB (I2C-like)
- **Default XCLK**: 20 MHz
- **Frame format**: JPEG
- **Frame buffers**: PSRAM-resident, count 2

### Pin Map (GOOUUU board)

| Pin Name | GPIO |
|----------|------|
| PWDN | -1 (not connected) |
| RESET | -1 (not connected) |
| XCLK | 15 |
| SIOD | 4 |
| SIOC | 5 |
| D0-D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |

### USB Mode

- **Mode**: USB-Serial/JTAG (default)
- **Device**: `/dev/ttyACM0`
- **Console**: ESP-IDF monitor via USB-Serial/JTAG

### Partition Plan (16 MB Flash)

| Partition | Offset | Size | Type |
|-----------|--------|------|------|
| nvs | 0x9000 | 24 KB | data/nvs |
| phy_init | 0xf000 | 4 KB | data/phy |
| ota_0 | 0x10000 | 5 MB | app/ota_0 |
| ota_1 | 0x510000 | 5 MB | app/ota_1 |
| otadata | 0xa10000 | 8 KB | data/ota |
| spiffs | 0xa12000 | 512 KB | data/spiffs |

### Peripherals

- **Flash LED**: GPIO 2, 3, or 46 (probed at boot)
- **Status LED**: Configured in `status_led.c`
- **No onboard mic**: Audio features not included
- **No SD slot**: Storage not included

## Scope

### IN Scope

- Camera capture and streaming (MJPEG, RTSP)
- AI detection (face, motion, QR)
- Web UI with full settings control
- ONVIF discovery and SOAP service
- AT command interface
- NVS configuration persistence
- Dual OTA partitions (firmware update ready)
- SPIFFS for web UI assets

### OUT Scope

- Audio recording/playback
- SD card storage
- H.264 video encoding
- 5 GHz WiFi
- ONVIF PTZ control
- NVR/NAS upload
- Cloud integration

## Key Design Decisions

### AI ↔ VGA Coupling

- AI pipeline hardcodes 640×480 buffers
- Non-VGA framesize disabled when any AI feature enabled
- Enforced in both web UI and REST API

### Coordinated Camera Reinit

- Framesize/quality changes stop AI + broadcaster → deinit camera → reinit → restart
- Prevents crashes from accessing invalid camera state

### Live vs. Persisted Settings

- Sensor settings (brightness/contrast/saturation/sharpness/mirror/flip): Applied live
- Framesize/quality: Requires coordinated reinit
- AI features: Applied live + persisted
- WiFi settings: Saved + device reboots

### Publisher-Subscriber Pattern

- `frame_broadcaster` publishes frames
- `mjpeg_streamer` and `ai_pipeline` subscribe
- Allows multiple consumers without frame duplication

## Camera limits measured (2026-09-04, on-board) — VGA-only module

**实测**（PIT-021 流程：web 热重配 + 冷启动双路径 + capture 计时）：
- VGA(10)：26fps 广播 / 0.33s capture / 零故障 —— **板级上限，已全链路锁定**
- SVGA(11)/XGA(12)：热重配后 capture 死（0B，间或出一帧）
- HD(13)+：`cam_hal: FB-OVF` 风暴、httpd 楔死；**冷启动存 HD 配置时传感器输出仍是
  VGA**（配置与实际脱节——GET 谎报 resolution 的隐患源）
落地：`CAMERA_RES_BOARD_MAX=10`（camera_driver.h）+ `camera_get_effective_max_res()`；
supported_resolutions/POST/AT+CAMRES/camera_init/camera_reinit/NVS 加载全部收敛 VGA。
推流仅 ~0.4fps：本板 ch11 HT40 弱态网络所致（TCP 窗口已提至家族值 49152/32768 无感、
AMPDU 重开实验无增益且伴一次失联——已回退 =n，调优候选是挪信道/关 HT40）。
**射频调优 2026-09-13 落地（.119 实测，PIT-053）**：① `esp_wifi_set_ps(WIFI_PS_NONE)`
入 wifi_manager（此前漏设=IDF 默认 MIN_MODEM 射频休眠，同负载 A/B：丢包 10%→3.3%、
RTT 均值 177→106ms——"信号强但页打不开"的头号根因）；② 强制 HT20 已试**并回退**
（双客户端拉流下每帧双倍空口时间，ping 52.5% 丢包，负优化，现跟随 AP 协商）；
③ 挪信道仍待 AP 侧动作。另：.30（NVR 查看端）+ .9（NVR 录像）双路拉流会把弱链路
打进失聪窗，属负载物理非固件可救。
NVS 观察项：连续 AT 改 AI 键后出现 `Failed to write NVS key 'ai_motion_enable'`（运行时生效、持久化失败）——待查 NVS 页空间。

## Camera quality bounds (2026-09-04, applied)

- `CAMERA_QUALITY_MIN/MAX = 10/63`（camera_driver.h，驱动不变量：esp32-camera
  JPEG fb 按 w*h/5 分配，q<10 复杂场景超预算截帧，PITFALLS PIT-021）。POST
  /api/camera 与 POST /api/config（白名单键）越界 400；camera init/reinit 钳制；
  GET /api/camera 新增 `quality_min/quality_max`（SPA 滑杆钳制，四仓 app.js 已同步）。
- 分辨率上限见上节：**SXGA**（2026-09-05 二次翻案，PIT-021 二次附录；旧 VGA-only/SVGA 归因均已推翻）。
- OTA 移植 WIP 已结清（eb65387 合入 ota_updater，ota:true，Web OTA 自测分区翻转通过）。
- 默认凭据 2026-09-05 轮换：RTSP digest `admin/mibeecam2026`（原 admin/admin）；
  AP 模式开放网络→WPA2 家族统一 `mibeecam2026`（原 authmode=OPEN）。存量设备 NVS
  保存的旧值不会自动迁移——需 POST /api/config 改存（本机 .119 已对齐）。
- RTSP 会话为内存上限约束（每会话 2×8KB 任务栈+缓冲，内部堆紧张时 ~2 并发即
  ENOSPC "Not enough space" 拒新连接，PIT-025 兜底已生效不炸机）——NVR 占满后
  手动探测会被拒，属已知 RAM 特性非回归。

## Do NOT

- Copy `partitions.csv` or pin numbers from the reference repos verbatim — flash size and board differ.
- Assume OV3660 behaves like OV2640 for frame size, JPEG quality, or XCLK frequency. Validate empirically.
- Set `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=n` (Octal PSRAM corruption).
- Skip `set-target esp32s3` — without it the build silently targets the wrong chip.
- Commit `sdkconfig`, `managed_components/`, or `build/`.
- Add features outside scope without discussion.

## Verification contract

- Camera change → `idf.py build` clean + flash + `idf.py monitor` shows `camera initialized` and at least one successful frame grab logged.
- `sdkconfig.defaults` change → `idf.py fullclean` then rebuild (stale `sdkconfig` will mask your edits).
- Partition change → re-flash the partition table at `0x8000`, not just the app.
- Web UI change → rebuild spiffs.bin and re-flash partition.
- REST API change → build + flash + test endpoints manually.

## 2026-09-04 上午：NVS 键名红线 + AI/VGA 污染链（PIT-022）
- **NVS 键 ≤15 字符**：`ai_motion_enable`(16) 曾令 `config_save()` 整体失败（遇错即返回），
  其后所有键永不落盘——"AT 关 AI 重启复活"即此。键已改 `ai_motion_en`（JSON 字段名不变）。
  at_command.c / web_server.c 写键的字符串必须与 config_manager.c `s_keys[]` 完全一致，
  config_set 未知键现在会打 WARN。
- **AI 与 VGA 强耦合是事实上的默认态**：AI 任一开启 → 加载钳制强制 VGA + POST 非 VGA 被拒。
  之前"本模组仅 VGA"的结论被"保存失败→AI 复活→强制 VGA"污染（PIT-021/022），
  分辨率上限复测中（CAMERA_RES_BOARD_MAX 临时 15，测毕定稿）。
- 帧尺寸校验改区间（`val > max` 拒绝），不再是单值锁定；AT+INFO 现在打印 `AI: face=.. motion=.. qr=..`。
- **RTSP 会话创建包 try/catch**（PIT-025）：线程耗尽抛 system_error 曾整机 abort（rst:0xc）。
- uptime 改 `esp_timer_get_time()`（64 位，tick 回绕免疫）。
- 统一 logo favicon.svg（四仓同 md5，PIT-026 的 reconfigure 纪律适用）。

## 2026-09-04 下午：双网络支持 + 分辨率上限双网复核

- **本板此前是四仓唯一单 WiFi**（主网弱态即失联无路可退）。已加：
  `wifi_ssid_2/wifi_pass_2`（NVS 键 ≤15 字符红线遵守）+ `AT+WIFI2=ssid,pass`
  （查询脱敏；`ssid,` 空串清除）+ 三层择优/转移：
  ① 开机双网快扫 RSSI 择优（强 ≥8dB 胜出，否则沿用 NVS `wifi_pref/last_net` 上次好网）；
  ② 关联后 12s 无 IP（DHCP 盲区）直接切网；③ 运行期连败 2 次切网、切换计数
  ≥6 防乒乓后转 AP 兜底。`/api/status` 新增 `wifi_net`/`current_ssid`。
  ⚠ 开机择优只在启动时——运行期"弱而不断"不迁移（无 roaming），需要时重启板子即可重选。
- **分辨率上限双网复核**：GT（主网）与 MiBeeAP2（备用网）上 VGA/SVGA 正常、
  XGA 冷启动采集死**完全一致**——上限 SVGA 与网络无关，维持。
  （⚠ 该"SVGA 板级极限"结论次日夜被推翻两次——见下方"SXGA 翻案"节：
  真因是 PSRAM 40MHz + 内部 DRAM 耗尽 + XCLK 20MHz，均与模组/DVP 无关。）
  **方法论纠正**：推流 delivered fps（0.5-0.8fps）是"链路 RTT/丢包 + NVR 双路订阅"
  的投递侧指标，同期板端采集 25-27fps（frame_broadcaster 日志）——**分辨率上限判定只看
  采集侧**（fb_get 是否出帧 + JPEG SOF 实测尺寸），勿用投递 fps 做依据。
- 实测 MiBeeAP2 板位 RTT 仍 ~50-220ms——.119 位置的射频环境两网都一般，
  网络调优（挪信道/关 HT40）仍是独立课题。

## 2026-09-04 分辨率三层上限（家族统一）+ 传感器身份纠偏

- **三层化**：`camera_get_effective_max_res() = min(sensor, board, memory)`
  （camera_driver.c，细节见 PITFALLS PIT-021 附录）。sensor 层查组件能力表
  （`esp_camera_sensor_get_info().max_size`，OV3660→QXGA=19）；board 层
  `CAMERA_RES_BOARD_MAX=11/SVGA`（双网实测，不变）；memory 层 PSRAM fb 预算
  （512K floor，只能收紧）。`GET /api/camera` 下发 `res_cap_source`；
  supported_resolutions 由静态表改为按 effective 循环生成；AT+CAMRES 同步。
  本板满配不变（10-11、source=board）。
- **传感器身份纠偏（顺带修复）**：本仓曾把 OV3660 的 PID 误记为 **0x77**
  （0x77 其实是 OV7725；组件对 OV3660 只认 **0x3660**——`ov3660_detect` 读
  0x300A/0x300B 比对）。后果：`camera_sensor_name()` 的手抄映射对实戴传感器
  返回 "unknown"，camera_init 的 "PID=0x77 confirmed" 分支永不命中。
  已改查组件表取名/确认；camera_init 不再硬拒 OV2640（换传感器由 sensor
  层自动收缩候选，符合家族"换板/换传感器自适应"方向）。硬件表中"Sensor ID:
  0x77"为误记，勿再引用。

## 2026-09-04 晚 API parity 补齐（契约 §4 违约修复，已烧录验证）

四板实测矩阵 × SPA 字段消费交叉核对后，本板补齐三个核心 status 字段
（用户报障"119 无信号显示"的根因即前两个缺失）：
- **`wifi_rssi`/`wifi_channel`**：`wifi_manager_get_rssi()/get_channel()`
  （`esp_wifi_sta_get_ap_info`，未连接返回 0）→ SPA 统计条信号芯片 +
  WiFi 页当前连接行。实测 -47dBm/ch2 正常下发。
- **`chip_temp`**：S3 片内温度传感器（`esp_driver_tsens`，CMake REQUIRES 已加），
  量程 (50,125)→(20,100)→(-10,80) 依次回退（跨档驱动拒绝，同 seeed 教训），
  **惰性安装于 web_server**（首次 /api/status 时装，实测 60°C）。
- ai-thinker 同轮补 `free_psram`（其板 4MB PSRAM）。剩余差异均为硬件/功能正当
  （经典 ESP32 无温度传感器、luatos 无 PSRAM、SD/录像/传感器微调随能力省略）。
- **timezone 不补**：本板无 NTP/时区应用路径（仅 /api/time 手动设 epoch），
  加字段即死字段——等有真实时区消费再随功能加。

## 2026-09-04 深夜：httpd 自愈误杀修复（2-4 分钟重启循环根因，已烧录验证）

**症状**：.119 每 2-4.5 分钟 `rst:0xc`（无 panic），串口签名
`httpd_accept_conn: error in accept (23)` → `httpd :80 probe failed (2/2)` →
`unresponsive for 120s — rebooting`。**注意 socket 上限不是原因**——本仓
`LWIP_MAX_SOCKETS=16`/`TCP_MSL=15000` 早已配置（与 ai-thinker/luatos 同款），
循环依旧。真因：**探针自愈无资源分类**——NVR 多路订阅挤占 lwIP 池时，探针自己
socket()/connect() 拿不到资源（EMFILE/ENOBUFS）也被计为"httpd 死"，2/2 即重启。
**修复（PIT-002 家族教训收尾，方案同 seeed/ai-thinker 两仓验证配方）**：
① 探针端：socket()/connect() 资源类失败打 WARN 并返回"不计数"，只有
"TCP 连上但应用层无响应"才计失败；② 调用端：WiFi 未连接时不计数。
**上板实证**：修复前 20:37-20:49 五连重启（2-4.5 分钟间隔）；修复后仅
20:53 一次**真卡死**正确自愈（探针 TCP 连上但 120s 无应用响应——这正是
该重启的场景），其后 16+ 分钟零重启、uptime 连续爬升。


## 2026-09-05 分辨率翻案至 SXGA（1280×1024）+ 三项连带修复

**结论：板上限 SVGA→SXGA**（`CAMERA_RES_BOARD_MAX=14`），XGA/HD/SXGA 冷启动+
热重配均 JPEG SOF 实证出图；UXGA init 失败（回滚+自愈重启路径已验证）。
SXGA 90s 探针：**投递 3.42fps**（308 帧/66KB 均值）——远超 SVGA 时代 0.4-0.8fps，
PSRAM 提速连带解锁了网络路径。旧"模组/DVP 组合极限"结论**两次都错**：

1. **PSRAM 实际跑在 40MHz**：defaults 注释称"VERBATIM from seeed"但
   `CONFIG_SPIRAM_SPEED_80M` 行**漏抄**（seeed=80M）。补上后 XGA init 不再
   `cam_dma_config: DMA buffer 16384 Byte malloc failed`——那才是"XGA 取帧死"
   的第一层真因（**内部 DRAM 耗尽**，最大空闲块 15.3KB/9.7KB，与 DVP 无关）。
2. **XCLK 20MHz 下 XGA+ 帧损坏**（`NO-SOI/NO-EOI/EV-EOF-OVF`）：内存修好后
   浮出的第二层真因。XCLK 降到 **16MHz** 后 XGA/HD/SXGA 全稳（`camera_driver.c`）。
3. 同时补抄 seeed 的 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`（WiFi/lwIP 缓冲
   迁 PSRAM，腾内部 DRAM——"verbatim"漏抄的第二行）。

**连带修复（同轮提交）**：
- **camera_reinit 失败路径回滚 bug**：旧代码先用 config 保存新档再 init，失败后
  从 config 读"刚保存的失败档"去恢复=无限重试死循环。改为留旧值回滚；回滚 init
  也失败则 1s 后自动重启（boot 路径不走 reinit，无重启环风险）。
- **AI 任务功能全关时不再解码**：此前无条件 sw_decode_jpeg——VGA 白烧 CPU，
  SXGA 下 RGB888 3.9MB 必然分配失败逐帧刷错。现解码前查功能开关。
- **ai_pipeline 全部 TWDT 交互移除**（IDLE1 摘除+每 ≤10ms yield 本就无饿死路径）。
- **espp__task 补丁**（`patches/espp__task/`，root CMake 拷贝步骤同 led_strip 模板）：
  删掉 thread_function 里全固件唯一的 `esp_task_wdt_reset` 调用点。

**task_wdt 洪水悬案——已破（2026-09-05 结案，PIT-028，修复 `680fb76`）**：
`esp_task_wdt_reset(707): task not found` 高频刷屏的真因是 **IDLE1 空闲钩子孤儿**：
本仓 ai_start_task 的 `esp_task_wdt_delete(IDLE1)` 只删订阅条目，不注销空闲钩子
（IDF 官方退订是 deregister_hook + delete 两步，task_wdt.c:286-287），IDLE1 随后
每轮空转调**被内联进钩子的** esp_task_wdt_reset → 条目已删 → "task not found"。
此前"全固件零静态引用"结论是**内联盲区**（符号尸体无人引用≠无人执行）。运行期
实锤手法：`--wrap=esp_rom_printf` 打印调用者 ra+任务名（v6 该配置下 ESP_LOGE 走
EARLY→esp_rom_printf 通道，不进 esp_log 族——wrap esp_log 挂空）。修复：
`sdkconfig.defaults` `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`（构建期不订阅，
官方姿势）+ 删除运行期 delete(IDLE1) 代码；上板验证洪水归零、SXGA 9.4fps 双订阅
流正常。**注意**：gitignored `sdkconfig` 里对应两行（ESP_ 与无前缀别名）也要手改，
勿 rm 重生成（会丢本地密码注入）。另：诊断探针勿 wrap ROM 的 `ets_printf`——启动
早期被调到会挂死进 RTCWDT boot loop（实测翻车）。

**sdkconfig 纪律提醒**：本仓生成 sdkconfig 现含 PSRAM 80M/WiFi-LWIP/本地密码
注入（gitignored）。rm sdkconfig 重配会丢密码注入——defaults 改动后应手改生成
文件或重配后回填 `CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD`。
## 2026-09-06：ESPectre CSI 运动感知（家族推广，本板=完美可用 ✅）

`components/espectre/`（GPL-3.0-only，与仓 LICENSE 一致）+ `main/csi_motion.*`
（第 3a 步，wifi_manager_init 后），`CONFIG_MIBEE_CSI_MOTION` 门控默认 n
（`select ESP_WIFI_CSI_ENABLED`；门关=零回归）。开门 +93.6KB（5MB 槽无压力）。
本板特有：csi_motion 任务**核 0**/8KB 栈（核 1 被 broadcaster+AI 占满曾致感知
静默停摆）；`wifi_manager.c` 的 `esp_wifi_set_config` 已改温和错误处理
（ESPectre 策略异步重连竞态曾触发 ESP_ERROR_CHECK abort 重启，实测复现一次）。
实测（ch2，真实 SSID 脱敏）：校准 OK(thr=0.32)、5min+ 连续心跳零重启、
4.99fps/206KB/s 并存零断连、2 次 MOTION。详见 PITFALLS PIT-034。

## 2026-09-08：ONVIF Pull-Point 事件服务（契约 v1.5，NVR 运动报警联动）

`main/onvif_events.c/h`（与 seeed 同源，仅 config/IP 取值两函数板级适配）：
CSI 运动 → `MotionAlarm` 通知，NVR `CreatePullPointSubscription` →
`PullMessages` 轮询。单订阅、1h 终止、120s 无拉取过期、无长轮询；事件生成由
config 键 `onvif_events`（默认 0）门控。**本板 GetCapabilities 此前就广告了
`/onvif/events_service` XAddr 但无实现**（NVR 一订阅就 fault）——本次做实。
CSI 扇出点在 `csi_motion.cpp` 的 `on_motion_state_changed`（本板此前该回调
仅打日志）。

- **坑（已修）**：`max_uri_handlers = NUM_URIS + 2` 的富余恰好被 2 个 ONVIF
  URI 吃满，第三个 ONVIF URI 注册即 `ESP_ERR_HTTPD_HANDLERS_FULL`——已改
  `NUM_URIS + 4`。加端点前先核。
- 探针 `tools/onvif_events_probe.py`；SPA 开关"ONVIF 运动报警（NVR 联动）"。

## 2026-09-08：CSI 状态 HTTP 回退（契约 v1.6，SPA 胶囊对齐 seeed）

本板 CSI 常开但**无 WS 服务**（`websocket:false`）——SPA 的 CSI 胶囊/统计片
此前只认 `/ws csi_status` 心跳，在本板 UI 全静默。v1.6 起 `GET /api/status`
带可选 `csi` 对象（与心跳同形同值），SPA 的 1Hz 轮询消费它驱动胶囊。
快照源 `csi_motion_get_status()`（`csi_motion.cpp` 内 portMUX 单写者快照，
`on_periodic_update` ~1Hz 写、httpd worker 读，读侧不阻塞感知回调）。

## 2026-09-13 部署：LED 修复上板（issue #11）+ PIT-050 + LED 稳定性实测

**背景**：用户报（.119）"灯亮了就几乎连不上 + POST /api/led 500"。板上实为
`5e3ab5a-dirty`（v0.1.0-test-49，2026-09-10 构建）——**缺 2985d1f**（LED init
补调用 + 护栏对齐 + socket 16→24 + PIT-037 悬垂防护）。500 复现签名
`Flash LED control failed` = LED 模块从未 init。

**OTA 险情（老固件）**：3.97MB 镜像前两传分别死在 ~2MB/95s 与全量末尾
（http=000 + 板复位、镜像未落地）——与 2985d1f 修的"churn 无护栏 90s 级重启
循环"时间尺度吻合。第 3、4 传全量 200 成功（新固件 OTA 通道此后一次过 86s）。
**教训：老固件 OTA 失败先重试两三次再降级 USB**。

**部署后实测**：`/api/led` 全亮度档 200（含 100/30/0）；**LED@100 + CSI 并存
+ 40×/api/status 连续探测 + /api/capture（48KB/0.31s）零复位、RTT 大多
<200ms**——"灯亮=连不上"在新固件不复现（老固件的 LED 无 init + churn 重启
循环共同制造的表象）。若用户侧仍复现，才查供电/线缆。

**自摆乌龙（PIT-007 变体）**：fresh 机器上跑 `idf.py set-target esp32s3`
**重新生成 sdkconfig**，把 gitignored 的 `MIBEE_CSI_MOTION=y` + 本地密码
覆盖回 defaults（症状：部署后 `/api/status` 无 `csi` 对象）。恢复自
`sdkconfig.old`（set-target 自动备份）。**sdkconfig 已存在时不要跑
set-target**；跑过必查 `sdkconfig.old`。

**遗留观察（新旧固件同表现，非回归）**：本机对 `:81/stream` 的连接
GET 后立即 RST（探针 0 帧×5 断连）；NVR（.30）的会话在（clients=1）。
待查：单槽互踢 vs 堆地板任务创建失败 vs RTSP 路径优先。

## 2026-09-14 flash_viewers 板级扩展移植（已实现+构建，两台存量单元部署受阻）

用户需求"有闪光灯的板都要有观看者驱动闪光灯"：家族四仓中仅 ai-thinker 与
本板有闪光灯（seeed/luatos 能力位 flash_led=false 无硬件，SPA 开关因 config
无键天然隐藏）。本板移植完成：

- `flash_viewers.c/h`（适配本板 API：`mjpeg_stream_client_count()` /
  `flash_led_get_brightness()` 判亮 / `config_get_flash_viewers()`）；
  1Hz 静态栈任务，观看者=流客户端>0 或 3s 内有 /api/capture，20s grace
  防频闪；默认关。
- config 走表驱动：结构体 `bool flash_viewers` + 默认 false +
  `KEY_ASSERT("flash_viewers")` + 表项 `{ TYPE_U8 }` + getter；web_server
  GET 加 bool、POST 白名单+0/1 校验分支、status 加 `flash_viewers:
  {enabled,active}`、api_capture_handler 加 notify 钩子；main.c 在
  `mjpeg_stream_server_start(81)` 后 start；CMakeLists 加源文件。
- 家族 SPA（四仓已同步）按"config 键在位"自动显示开关，本板固件上线即用。
- `idf.py build` 通过（v6.0.1，bin 4.07MB < ota 槽 5MB；spiffs 512KB）。

**部署受阻（两台在线单元 .113/.119 均跑 v0.1.1 test-54/55）**：网络 OTA
两次尝试均在 ~2min 处失败（1.5MB/12.5KB/s 后连接断、设备重启）——v0.1.1
的 health 自愈在上传占死 httpd 时连败 6 探针 → esp_restart（PIT 自愈误杀
家族 bug 的老固件表现，新固件已修，鸡生蛋）。迁移风险已评估为低（首基线
即逐键 NVS，wifi_ssid/wifi_pass/web_password 键名贯穿全历史，缺失键=默认
值，AI 键改名有 lazy 迁移），但传输层过不去。**解锁路径**：两台上 USB
（原生 USB-OTG 口，PIT-044 注意 DTR/RTS）后 `idf.py -p <口> flash`，或
短期大量重试碰运气（成功率低）。build 产物已就绪（build/mibee_cam.bin +
build/spiffs.bin，含本移植与四仓 SPA 同步版）。

## 2026-09-17 双单元运维事实 + 端口身份教训（误刷事故记录，issue #23）

- **两台在线单元**：`.119`（OV3660，CH340 → `/dev/ttyUSB0`，采集器常驻口）；
  `.113`（**实载 OV5640**，原生 USB-JTAG → `/dev/ttyACM0`，无 AT 控制台——AT 只在
  CH340 那台的 UART0 上）。
- **USB 刷写前必须抓启动 banner 验板型**：by-id 名 `Espressif_USB_JTAG_*` 是所有
  S3 原生 JTAG 板共有，不是 XIAO 指纹。2026-09-17 曾据此把 seeed 固件 + 8MB 分区表
  误刷到 .113（症状：fw 0.3.0/api 1.8 + 摄像头 init 失败——XIAO 引脚表不配 GOOUUU），
  后用本仓镜像 USB 全量重刷恢复（16MB 分区表复原，NVS 无损）。副作用：.113 从
  v0.1.0-test-49 直接升到当日 main，flash_viewers 移植随之落地（已开，NVS 持久）。
- **.113 网络身份以启动日志为准**：`wifi_manager: WiFi connected, IP: ...`
  （或 `esp_netif_handlers: sta ip:`）——掉线重入/换网窗口期旧地址会暂时失联，
  找板先看这个再扫网段。
- **ch7 拥塞窗口（主网 AP 当前自动信道）**：busy≈75% 时段 .113 会出现 ping 100% 丢
  而 TCP 慢通、`TrafficGen errno=12`、CSI `cb_pps` 短暂归零的 TX 窘迫——自愈型
  （复位加速恢复；双板 CSI 最终都完成校准，cb≈37/286pps）。根治靠 AP 挪信道
  （用户侧动作），勿当固件回归排查。判别基准：`.119` 同窗 ping 正常。
- 采集器（`overnight_log.py /dev/ttyUSB0`）本日重启过一次——旧进程自 09-13 楔死
  （USB 重枚举盲区已知坑），日志断档 4 天属正常现象。

## 2026-10-08：STA 连接兼容性阶梯（p0-p4）+ netif 生命周期重构

**需求源头**：台面 N16R8（MAC be:5c，实载 OV5640）被主路由 2.4G 拒收——用户指令"不让路由适配主板，让主板兼容能力更广泛更强"。

### 连接兼容性阶梯（wifi_manager.c）

五档、逐级降级、整轮失败才升档（升档=换姿势把两网重试一轮，不是每断连就换）：

| 档 | 射频/关联参数 | 靶向 |
|---|---|---|
| p0-default | bgn / 带宽跟随协商 / 无 802.11k v（=历史行为，零回归） | 基线 |
| p1-steer | +802.11k(rrm)/v(btm)/MBO + 全信道扫描 + failure_retry_cnt=3 硬敲门 | "AI 漫游引导"型路由（assoc 成功即 ASSOC_LEAVE 踢人的典型对策） |
| p2-ht20 | bgn + 强制 HT20 | HT40 协商完不成/邻道干扰环境 |
| p3-legacy | 纯 11b/g + HT20 | 关联 IE 不带 11n HT，最大兼容面 |
| p4-randmac | p3 射频 + 每 SSID 稳定派生本地管理 STA MAC（FNV-1a(base_mac+ssid)，同网重启不变） | 路由器按 MAC 黑名单/防攻击抑制（换身份敲最后一次门） |

- 升档三入口共用 `sta_ladder_escalate()`（断线重试上限/切换上限/监视器超时），`s_sta_profile` 单调递增防越级；耗尽才 AP 兜底，兜底被禁则停最底档轮换。
- **活动宽限（PASS_GRACE_MS=120s）**：监视器在"距最近连接请求 <120s"时只观望不做兜底决策——否则旧窗口会把刚升档的新一轮 2s 内兜底掉（台面实测 p0→p1 升档即被旧窗口掐死）。p1+ 一整轮可达 90s+（全信道扫描+3 次重试）。
- p4 档 STA_START 跳过双网重选扫描（MAC 按升档时的网络派生，重选会造出 MAC↔网络错配）；30min 周期重试（PIT-060）从头走阶梯。
- `/api/status` 加性字段 `wifi_compat_profile`（消费者可忽略）。

### 台面实测结论（2026-10-08，两网真实 SSID 已脱敏=主网 AP/主路由）

- 五档**全部被拒**（p4 换新 MAC 同样拒）→ 该路由器 2.4G 此刻拒**一切新关联**（客户端数上限/白名单/射频卡死类），非客户端参数可解；PC 同密码 5G 可连佐证密码与 SSID 配置无误。等路由器侧状态变化，30min 重试自动收容。
- 备用网（主路由）台面扫描只见 5GHz BSSID（已脱敏）——S3 永远连不上，双网择优里它是摆设（部署位可能有 2.4G，换备用网前先扫；后文深夜续有更正：2.4G 存在但广播 WPA3-SAE）。
- 阶梯机制本身全程验证：五档逐级应用（p3 proto=0x3 ✓、p4 MAC ✓）、宽限防抢跑 ✓、耗尽文案 ✓、干净落 AP ✓。

### netif 生命周期重构（存量崩溃根除，PIT 级）

- **根因**：esp_wifi_stop() 的 `WIFI_EVENT_STA_STOP` 默认 action（esp_netif_action_stop）还排在事件循环队列里，旧的"切换即 destroy STA netif"让 action 落到已释放对象 → lwip `netif_ip6_addr_set: invalid netif` assert 间歇性重启。**test-69 起就存在**（backtrace 落 esp_wifi_register_if_rxcb 邻域，15:32 台面复现实锤），此前"AP 兜底后莫名重启"多属此。
- **修复**：双 netif（STA+AP）在 wifi_manager_init 一次性创建、永不销毁；AP 静态 IP 在 init 配好（set_ip 撞 DHCP_NOT_STOPPED→stop 重试；末尾 dhcps_start 在 netif 未 up 时=状态归位 INIT，恰好重新武装 AP_START 默认 action 的自动拉起）；AP↔STA 只 stop/set_mode/start。esp-idf 官方 AP/STA 例程同款姿势。
- ⚠ init 时对默认 AP netif 直接 `ESP_ERROR_CHECK(esp_netif_set_ip_info)` 会因 DHCP 已在跑而 abort 开机循环——必须走上面的免疫式序列。
- 重构过程中曾加的 100ms 驱动静默 delay 已随本重构删除（netif 不销毁后无必要）；switch_to_ap 后落到的裸 `esp_wifi_connect()` 已加 `s_ap_started` 守卫（falls-through 曾撞已拆 netif）。

### 家族推广 TODO

阶梯（sta_ladder_escalate + 五档 + 宽限）与 netif 重构需按"同构→拷贝改"推广至 seeed-esp32s3-cam / ai-thinker-esp32-cam / luatos-esp32s3-a10-camera；推广时 wifi_manager 各板已有分叉（ap_sta_retry 只有部分仓有），逐仓适配勿盲拷。

### 2026-10-08 深夜续：隔离测试定性（本台单元硬件嫌疑）+ p5-wpa2

- **拓扑更正（用户提供）**：备用网=主路由（GT-3000 型号），主网=级联的另一台
  TP-Link AP——两台独立设备（真实 SSID 已脱敏）。NVS 备用网密码（已脱敏）经 PC WPA3(H2E) 验证有效。
- **主路由 2.4G（BSSID 已脱敏）广播 WPA3-SAE**，与相机 SAE 交互失败（auth 全
  超时）→ 阶梯加 **p5-wpa2**：pmf capable=false（协议上放弃 SAE）+ threshold=WPA2_PSK，
  过渡模式 BSS 直接走 WPA2。实测主路由仍不收（其 2.4G 疑似 SAE-only）。
- **用户关键事实**：ai-thinker / seeed / 另一台 N16R8 在同路由上全部正常——"路由器拒一切"
  结论作废，问题锁定本台单元特有。
- **隔离测试（PC Windows 移动热点 MiBeeTest，2.4G，1m 距离）**：关联后 4-way 握手超时
  （reason=15/201），**全新热点、首次见面、无黑名单可能** → 失败与路由器无关。
- **NVS 全清重测**（见下节事故说明，NVS 被整片擦除）：全新 NVS + 全新 RF 校准 → 仍连不上
  任何 AP。软件变量至此穷尽：凭据（PC 双网验证）、NVS 状态、RF 校准、MAC（随机化）、
  协议参数（六档）、固件版本（9-24 旧版与当日新版）、四台不同 AP。
- **定性**：本台单元 STA 射频路径在"关联后的握手/数据交换"层失败（mgmt 帧能通、扫描
  正常、自家 AP 模式正常），指向**硬件**（射频前端/晶振/USB 供电 sag 嫌疑）。建议：
  换 USB 线/直插主板口复测（供电 sag 一分钟排除）→ 查天线座 → 仍不行则按硬件处置。
  该单元 9 月中旬曾在线推流正常，属渐进劣化。
- **运维状态变更**：NVS 被擦后两网 WiFi 凭据已回填（真实密码已脱敏，记录在本地
  安全处）；**web_password 回到默认 mibeecam2026**（原为用户个人口令，已脱敏，联网后 POST /api/config 可改回）；
  flash_viewers 开关被重置（默认关，联网后重新打开）。

### 2026-10-08 事故记录：esptool v5 erase_region 参数语义（PIT 级）

`esptool v5 erase_region <offset> <size>` 是**偏移+长度**（不是老版的起止地址）。
本次 `erase_region 0x9000 0xf000` 实擦 0x9000..0x78000，把 ota_0 镜像前 416KB 一并擦掉，
板子进 "No bootable app partitions" 启动循环。恢复：全量重刷（bootloader+分区表+app+
otadata+spiffs，serialtap flash 一次过）。教训：**v5 对 region 类命令优先怀疑参数语义**，
擦分区前先 `serialtap partitions` 核对偏移，或用 serialtap 分区感知命令替代裸 esptool。
