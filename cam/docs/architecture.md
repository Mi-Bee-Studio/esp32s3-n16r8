# Architecture

## Module Map

| Module | Files | Purpose | Dependencies |
|--------|-------|---------|--------------|
| main | main.c | App entry, boot sequence orchestrator | All modules |
| config_manager | config_manager.c/h | NVS-backed configuration with FreeRTOS mutex, TYPE_U8/TYPE_I8 key types, 16 keys total | nvs_flash |
| camera_driver | camera_driver.c/h | OV3660 camera init from config, sensor settings application, coordinated reinit | esp_camera, config_manager |
| frame_broadcaster | frame_broadcaster.c/h | Frame grab task on Core 1 → subscriber publish pattern | esp_camera, mjpeg_streamer, ai_pipeline |
| mjpeg_streamer | mjpeg_streamer.c/h | HTTP MJPEG streaming via chunked multipart | frame_broadcaster |
| ai_pipeline | ai_pipeline.cpp/h | Face detection + motion detection + QR decode on Core 1, 640×480 hardcoded buffers | esp-dl, quirc, frame_broadcaster |
| web_server | web_server.c/h（路由表+生命周期）+ web_api_{system,config,camera,ai}.c + web_server_{util,static}.c | REST API (7 endpoint groups) + SPIFFS static file serving（issue #19 按域拆分） | esp_http_server, config_manager, flash_led, ai_pipeline |
| web_ui | index.html, style.css, app.js, i18n.js | Browser-based web interface | web_server (via SPIFFS) |
| wifi_manager | wifi_manager.c/h | WiFi STA mode connection | esp_wifi |
| flash_led | flash_led.c/h | GPIO flash LED control via LED strip driver | esp_driver_gpio |
| at_command | at_command.c/h | Serial AT command interface for configuration | esp_driver_uart |
| rtsp_server | rtsp_server.cpp/h | RTSP server with MJPEG-only streaming | frame_broadcaster |
| onvif_service | onvif_service.c/h | ONVIF SOAP service implementation | mdns |
| onvif_discovery | onvif_discovery.c/h | ONVIF WS-Discovery protocol | mdns |
| status_led | status_led.c/h | GPIO status LED | esp_driver_gpio |

## Boot Sequence

From `main.c`:

1. **nvs_flash_init** - Initialize NVS (or erase and retry on corruption)
2. **config_load** - Load configuration from NVS namespace "mibee_cfg"
3. **wifi_manager_init** - Start WiFi manager (connect to configured SSID)
4. **camera_init** - Initialize OV3660 from config:
   - Frame size and quality from config
   - VGA forced when AI is enabled
   - Sensor settings (brightness/contrast/saturation/sharpness/mirror/flip) applied
5. **frame_broadcaster_init** - Initialize frame broadcaster (publisher-subscriber)
6. **mjpeg_streamer_init** - Initialize MJPEG streaming infrastructure
7. **frame_broadcaster_start** - Start frame grab task on Core 1 (priority 5, stack 4096)
8. **ai_init** - Initialize AI pipeline:
   - Load face detection model from flash rodata
   - Allocate grayscale buffers in PSRAM (640×480)
   - Create quirc decoder instance
   - Start AI task on Core 1 (priority 5, stack 24576)
   - Apply config-based feature enables
9. **web_server_start** - Start HTTP server on port 80 (stack 16384)
10. **rtsp_start** - Start RTSP server (MJPEG-only, digest auth)
11. **onvif_start** - Start ONVIF discovery + SOAP service
12. **at_command_init** - Start AT command listener on UART0
13. **Idle loop** - 60-second heartbeat logging

## Data Flow Diagram

```
OV3660 Sensor
  ↓ (SCCB I2C)
esp_camera_fb_get()
  ↓ (frame_broadcaster_task, Core 1, priority 5)
publish_frame()
  ├─→ mjpeg_streamer (HTTP MJPEG chunked response)
  └─→ ai_pipeline (face/motion/QR detection)
       ↓ (JPEG decode → AI processing)
       ai_result_t (mutex-protected)
         ↓ (GET /ai/status polling)
       web canvas overlay (green boxes for faces, motion score, QR text)
```

## Task/Core Allocation

| Core | Tasks | Stack | Priority |
|------|-------|-------|----------|
| **Core 0** | main loop, WiFi events, HTTP server, AT commands, flash LED, status LED | 8192 | Default (1) |
| **Core 1** | frame_broadcaster, ai_pipeline | 4096 / 24576 | 5 |

## Memory Layout

### Frame Buffers
- Location: PSRAM (`CAMERA_FB_IN_PSRAM`)
- Count: 2 (`fb_count=2`)
- Format: JPEG (`PIXFORMAT_JPEG`)
- Size: Depends on framesize (VGA = 640×480, HD = 1280×720, etc.)

### AI Pipeline
- Grayscale buffers: PSRAM
- Dimensions: Fixed 640×480 (`AI_FRAME_W=640`, `AI_FRAME_H=480`)
- Face detection model: Flash rodata (`CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA=y`)
- Quirc decoder: Heap (internal DRAM)

### Configuration
- Namespace: "mibee_cfg"
- Storage: NVS
- Keys: 16 total (TYPE_U8 and TYPE_I8)
- Mutex: FreeRTOS mutex for thread-safe access

### Stack Sizes
- main: 8192
- frame_broadcaster: 4096
- ai_pipeline: 24576
- AT command: 4096
- web_server: 16384

## Module Relationships

```
main.c
├─→ config_manager (load/save)
├─→ wifi_manager (init)
├─→ camera_driver (init/reinit)
│   └─→ esp_camera
├─→ frame_broadcaster (init/start)
│   ├─→ camera_capture
│   ├─→ mjpeg_streamer (subscribe)
│   └─→ ai_pipeline (subscribe)
├─→ web_server (start)
│   ├─→ mjpeg_streamer (register handler)
│   ├─→ flash_led (control)
│   ├─→ ai_pipeline (get results)
│   └─→ SPIFFS (static files)
├─→ rtsp_server (start)
│   └─→ frame_broadcaster (subscribe)
├─→ onvif_service (start)
│   └─→ mdns (announce)
└─→ at_command (init)
```

## Key Design Patterns

### Publisher-Subscriber
- `frame_broadcaster` publishes frames to subscribers
- `mjpeg_streamer` and `ai_pipeline` subscribe to frames
- Allows multiple consumers without frame duplication

### Coordinated Reinitialization
- `camera_reinit()` coordinates stopping AI + broadcaster → deinits camera → reinits → restarts
- Prevents crashes from accessing invalid camera state during reinit

### Live vs. Persisted Settings
- Sensor settings (brightness/contrast/saturation/sharpness/mirror/flip): Applied live via sensor registers
- Framesize/quality: Requires coordinated camera reinit
- AI features: Applied live via `ai_enable()` + persisted to config
- WiFi settings: Saved + device reboots to apply

### AI ↔ VGA Coupling
- AI pipeline hardcodes 640×480 buffers
- Non-VGA framesize disabled when any AI feature is enabled
- Web UI enforces this coupling in both directions