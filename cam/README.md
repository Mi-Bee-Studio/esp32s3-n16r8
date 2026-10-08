# MiBee Cam

ESP32-S3-N16R8 + OV3660 camera firmware with MJPEG streaming, AI detection, RTSP, ONVIF, and responsive web UI.

## Hardware

- **Module**: ESP32-S3-WROOM-1 **N16R8** (16 MB Quad Flash, 8 MB Octal PSRAM)
- **Camera**: **OV3660** (3 MP, 1/5" sensor, max QXGA 2048×1536)
- **USB**: USB-Serial/JTAG (enumerates as `/dev/ttyACM0`)

### Board Overview

| Item | Value |
|------|-------|
| Board | GOOUUU ESP32-S3-CAM N16R8 carrier (pin map below is the GOOUUU wiring — other N16R8 boards may differ) |
| Module | ESP32-S3-WROOM-1 **N16R8** — Xtensa LX7 dual-core @ 240 MHz |
| Flash | 16 MB Quad SPI |
| PSRAM | 8 MB **Octal** (`SPIRAM_MODE_OCT` — NOT Quad; frame buffers live here, `fb_count=2`) |
| Wireless | 2.4 GHz WiFi b/g/n + BLE 5 |
| USB | USB Type-C — USB-Serial/JTAG (console + flashing, no bridge chip) |
| Camera | OV3660, sensor PID `0x77`, JPEG output |
| Partitions | Dual OTA slots in the 16 MB flash |

### Pinout Diagram (USB-C pointing up, front/component-side view; GOOUUU board)

```
                 ┌─ USB-C ─┐
   Camera (DVP) → │ ESP32-S3 │ ← TF / module antenna area
                 │  WROOM-1 │
                 │  N16R8   │
                 └──────────┘
  Camera (SCCB) → SIOD=IO4 · SIOC=IO5          XCLK=IO15
  Camera bus    → D0=IO11 · D1=IO9 · D2=IO8 · D3=IO10 · D4=IO12
                  D5=IO18 · D6=IO17 · D7=IO16
  Camera timing → VSYNC=IO6 · HREF=IO7 · PCLK=IO13
  PWDN/RESET    → not connected (-1)
```

Full pin map, PSRAM constraints and the partition plan: [docs/hardware.md](docs/hardware.md).

## Firmware Baseline Norms

Two baselines are mandatory fleet-wide for every MiBee firmware repo:

1. **Watchdog: mandatory.** ✅ This firmware: ESP-IDF task watchdog (TWDT 10 s,
   panic on timeout) with named per-task registration (`watchdog_register_current`)
   and periodic feeding (`watchdog_feed_current`).
2. **Web/API firmware upgrade (OTA): mandatory where the hardware allows.**
   ✅ This firmware: dual OTA slots + the `/api/ota` family
   (`/api/ota/upload`, `/api/ota/info`, `/api/ota/spiffs`) plus `esp_https_ota`
   pull-style updates; wired flashing (serialtap/esptool) remains the recovery
   path, not a substitute.

## Features

- 📷 **MJPEG streaming** — real-time video via HTTP
- 🤖 **AI detection** — face, motion, QR code with live web overlay
- 📡 **RTSP server** — MJPEG-only streaming with digest auth
- 🔍 **ONVIF discovery** — network camera discovery protocol
- 💡 **Web UI** — zh/en i18n, light/dark theme, full settings control
- ⌨️ **AT commands** — serial configuration interface
- 🚦 **OTA-ready** — dual OTA partition layout

## Quick Start

```bash
# Install ESP-IDF v6.0.1
git clone --recursive https://github.com/espressif/esp-idf.git ~/.espressif/esp-idf
cd ~/.espressif/esp-idf
git checkout v6.0.1
git submodule update --init --recursive
./install.sh esp32s3

# Activate ESP-IDF (every new shell)
source ~/.espressif/esp-idf/export.sh

# Clone and build
git clone https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam.git
cd esp32s3-n16r8-cam
idf.py set-target esp32s3
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor

# Open http://<device-ip>/ in a browser
```

## Documentation

- [Architecture](docs/architecture.md) — module map, boot sequence, data flow
- [Hardware](docs/hardware.md) — pin map, PSRAM constraints, partition plan
- [Web API](docs/web-api.md) — REST endpoint reference
- [Web UI](docs/web-ui.md) — UI features, i18n, theme, settings
- [Development](docs/development.md) — build, flash, CI, contributing

### Reviewing the code? Start here

- `docs/architecture.md` — module map, dependencies, boot sequence, data flow
- `main/web_server.c` — the complete HTTP surface in one `s_uris[]` route table near the top of the file (reading map in the file header)
- `docs/api-contract.md` · `docs/config-contract.md` · `docs/at-command.md` — versioned behavior contracts shared across the MiBee Cam family
- `docs/PITFALLS.md` — the family incident library behind every defensive workaround in this codebase (sanitized public edition)

## Project Status

This is a production-ready firmware with:
- ✅ Working camera (OV3660)
- ✅ MJPEG streaming
- ✅ AI pipeline (face, motion, QR)
- ✅ Web UI (zh/en, light/dark)
- ✅ RTSP server
- ✅ ONVIF discovery
- ✅ AT command interface
- ✅ NVS configuration
- ✅ Dual OTA partitions

## Known Limitations

- AI requires VGA resolution (640×480)
- RTSP is MJPEG-only (no H.264)
- WiFi is 2.4 GHz only (no 5 GHz)
- Web UI requires modern browser (ES6+)

## License

GPL-3.0-or-later

## Contributing

Contributions are welcome! Please see [Development](docs/development.md) for guidelines.

## Support

- Issues: [GitHub Issues](https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam/issues)
- Documentation: [docs/](docs/)
- AGENTS.md: Agent instructions for AI-assisted development