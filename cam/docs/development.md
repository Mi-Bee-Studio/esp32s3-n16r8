# Development

## Prerequisites

### Required Tools

| Tool | Version | Notes |
|------|---------|-------|
| **ESP-IDF** | **v6.0.1 (pinned)** | Do NOT use other versions |
| **Python** | 3.14+ | Required for ESP-IDF build system |
| **Git** | Any | For cloning and version control |

### System Requirements

- Linux, macOS, or Windows (WSL2 recommended on Windows)
- 8 GB RAM minimum
- 2 GB free disk space for build artifacts

## Setup

### 1. Install ESP-IDF v6.0.1

```bash
# Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/.espressif/esp-idf
cd ~/.espressif/esp-idf
git checkout v6.0.1
git submodule update --init --recursive

# Install tools
./install.sh esp32s3

# (On Windows)
# install.bat esp32s3
```

### 2. Activate ESP-IDF Environment

**Every new shell session**:

```bash
source ~/.espressif/esp-idf/export.sh

# (On Windows with cmd.exe)
# %USERPROFILE%\.espressif\esp-idf\export.bat

# (On Windows with PowerShell)
# . %USERPROFILE%\.espressif\esp-idf\export.ps1
```

### 3. Clone the Project

```bash
git clone https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam.git
cd esp32s3-n16r8-cam
```

### 4. Set Target

```bash
idf.py set-target esp32s3
```

**Critical**: This sets up the correct toolchain and configuration for ESP32-S3. Skipping this will cause silent build failures.

## Building

### Standard Build

```bash
idf.py build
```

Output:
- `build/mibee_cam.bin` - Main firmware
- `build/bootloader/bootloader.bin` - Bootloader
- `build/partition_table/partition-table.bin` - Partition table
- `build/ota_data_initial.bin` - Initial OTA data
- `build/spiffs.bin` - SPIFFS filesystem image

### Clean Build

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

Use when:
- Changing `sdkconfig.defaults`
- Switching target
- Experiencing mysterious build errors

### Incremental Build

```bash
idf.py build
```

Use for:
- Small code changes
- Testing iterations
- Normal development

## Flashing

### Identify Serial Port

```bash
ls /dev/ttyACM*
# or
ls /dev/serial/by-id/
```

Expected: `/dev/ttyACM0` (USB-Serial/JTAG)

### Full Flash (All Partitions)

```bash
idf.py -p /dev/ttyACM0 flash
```

This flashes:
- Bootloader (0x0)
- Partition table (0x8000)
- Firmware (0x10000)
- OTA data (0xa10000)
- SPIFFS (0xa12000)

### App-Only Flash (Fast Iteration)

```bash
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 build/mibee_cam.bin
```

Use for:
- Rapid testing during development
- Firmware-only changes
- Skipping bootloader/partition table

**Note**: Verify offset matches `ota_0` in `partitions.csv`

## Monitoring

### Start Monitor

```bash
idf.py -p /dev/ttyACM0 monitor
```

### Flash + Monitor in One Command

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### Monitor Shortcuts

- `Ctrl+]` - Exit monitor
- `Ctrl+T Ctrl+R` - Reset device
- `Ctrl+T Ctrl+H` - Show help

## Project Structure

```
esp32s3-n16r8-cam/
├── main/                      # Main component
│   ├── main.c                 # App entry point
│   ├── config_manager.c/h     # NVS configuration
│   ├── camera_driver.c/h      # OV3660 wrapper
│   ├── frame_broadcaster.c/h  # Frame publisher
│   ├── mjpeg_streamer.c/h     # HTTP MJPEG stream
│   ├── ai_pipeline.cpp/h      # AI processing
│   ├── web_server.c/h         # REST API server
│   ├── wifi_manager.c/h       # WiFi management
│   ├── flash_led.c/h          # Flash LED control
│   ├── at_command.c/h         # Serial AT commands
│   ├── rtsp_server.cpp/h      # RTSP server
│   ├── onvif_service.c/h      # ONVIF SOAP service
│   ├── onvif_discovery.c/h    # ONVIF WS-Discovery
│   ├── status_led.c/h         # Status LED
│   ├── web_ui/                # Web UI assets
│   │   ├── index.html
│   │   ├── style.css
│   │   ├── app.js
│   │   └── i18n.js
│   ├── CMakeLists.txt         # Component build
│   └── idf_component.yml      # Component dependencies
├── docs/                      # Documentation
│   ├── architecture.md
│   ├── hardware.md
│   ├── web-api.md
│   ├── web-ui.md
│   └── development.md
├── .github/workflows/         # CI/CD
│   └── release.yml            # Tag-triggered release
├── partitions.csv             # Custom partition table
├── sdkconfig.defaults         # Hardware configuration
├── CMakeLists.txt             # Project build
├── AGENTS.md                  # Agent instructions
└── README.md                  # Project README
```

## Configuration

### sdkconfig.defaults

**Single Source of Truth** for:
- Hardware pin map (`CONFIG_CAMERA_PIN_*`)
- PSRAM configuration (Octal mode)
- CPU frequency
- Flash size
- Partition table selection

**Do NOT**:
- Put hardware config in `Kconfig.projbuild`
- Commit `sdkconfig` (generated file)
- Copy pin maps from reference repos

### sdkconfig

- **Auto-generated** from `sdkconfig.defaults`
- **Gitignored** - do NOT commit
- Contains all ESP-IDF configuration options
- Run `idf.py fullclean` to regenerate

## Conventions

### Code Style

- **C Modules**: Flat layout, one `.c/.h` pair per subsystem
- **C++ Modules**: `.cpp/.h` for ESP-DL integration
- **Naming**: `module_function()` for public APIs
- **Headers**: Guarded with `#pragma once`

### Commit Messages

**Conventional Commits** format:

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Types**:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `refactor`: Code refactoring
- `test`: Tests
- `chore`: Build/tooling

**Examples**:
```
feat(camera): add framesize validation

Reject non-VGA framesize when AI enabled to prevent
buffer overflow in AI pipeline.

Fixes #42
```

```
fix(web): handle AI pipeline not running

Return 404 instead of crashing when /ai/status is called
but AI pipeline is not initialized.
```

### File Organization

**main/**:
- Keep all main component files here
- Use subdirectories for logical grouping (e.g., `web_ui/`)

**docs/**:
- All documentation in Markdown
- Keep sections focused and cross-referenced

## Component Dependencies

From `main/idf_component.yml`:

```yaml
dependencies:
  espressif/esp32-camera: "^2.1.6"
```

From `main/CMakeLists.txt`:

```cmake
REQUIRES esp-dl human_face_detect quirc nvs_flash spiffs
        esp_driver_gpio esp_driver_uart esp_wifi esp_netif esp_event
        esp_http_server espressif__cjson mdns
```

## CI/CD

### GitHub Actions

**File**: `.github/workflows/release.yml`

**Trigger**: Git tags matching `v*`

**Container**: `espressif/idf:v6.0.1`

**Steps**:
1. Checkout code
2. Build firmware
3. Package artifacts
4. Create GitHub release

**Release Artifacts**:
- `mibee_cam.bin` - Main firmware
- `bootloader.bin` - Bootloader
- `partition-table.bin` - Partition table
- `ota_data_initial.bin` - OTA data
- `spiffs.bin` - SPIFFS filesystem
- `flash_command.txt` - Pre-assembled flash command

## Testing

### Manual Testing Checklist

- [ ] Camera captures frames at VGA
- [ ] MJPEG stream accessible via browser
- [ ] AI features toggle on/off
- [ ] Face detection shows green boxes
- [ ] Motion detection shows score
- [ ] QR decode shows codes
- [ ] Camera settings apply live
- [ ] Framesize change triggers reinit
- [ ] WiFi save reboots device
- [ ] Web UI theme toggle works
- [ ] Web UI language toggle works
- [ ] RTSP stream accessible via VLC
- [ ] ONVIF discovery finds device

### Automated Testing

**Not yet implemented** - roadmap item for future releases.

## Troubleshooting

### Build Failures

**Problem**: "Wrong chip selected"

**Solution**:
```bash
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

**Problem**: "sdkconfig stale"

**Solution**:
```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

### Flash Failures

**Problem**: "Permission denied /dev/ttyACM0"

**Solution**:
```bash
# Add user to dialout group (Debian/Ubuntu)
sudo usermod -a -G dialout $USER

# Add user to uucp group (Arch)
sudo usermod -a -G uucp $USER

# Logout and login again
```

**Problem**: "Failed to connect"

**Solution**:
- Check USB cable (use data cable, not charge-only)
- Try different USB port
- Press BOOT button while powering on

### Runtime Issues

**Problem**: "Camera init failed"

**Solution**:
- Verify pin map in `sdkconfig.defaults`
- Check camera power (3.3V)
- Probe OV3660 sensor ID (expect 0x77)

**Problem**: "WiFi not connecting"

**Solution**:
- Check SSID and password in config
- Verify 2.4 GHz WiFi (5 GHz not supported)
- Check signal strength

**Problem**: "AI not working"

**Solution**:
- Ensure framesize is VGA (value 10)
- Check PSRAM is enabled (CONFIG_SPIRAM=y)
- Verify 64B cache line (CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y)

## Contributing

### Workflow

1. Fork the repository
2. Create a feature branch
3. Make changes
4. Test thoroughly
5. Commit with conventional commits
6. Push to fork
7. Create pull request

### Pull Request Checklist

- [ ] Code builds without warnings
- [ ] Manual testing complete
- [ ] Documentation updated
- [ ] Commit messages follow conventions
- [ ] No trailing whitespace
- [ ] No debug prints in final code

## Additional Resources

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/)
- [esp32-camera Component](https://github.com/espressif/esp32-camera)
- [ESP-DL Library](https://github.com/espressif/esp-dl)
- [Quirc QR Decoder](https://github.com/dlbeer/quirc)