# Hardware

## ESP32-S3-WROOM-1 N16R8 Module

| Specification | Value |
|---------------|-------|
| **SoC** | ESP32-S3, Xtensa LX7 dual-core @ 240 MHz |
| **Flash** | 16 MB Quad SPI |
| **PSRAM** | 8 MB Octal (NOT Quad) |
| **USB** | USB-OTG + USB-Serial/JTAG |
| **Serial Port** | `/dev/ttyACM0` (USB-Serial/JTAG) |
| **Baudrate** | 115200 |

### CPU

- Dual-core Xtensa LX7 @ 240 MHz
- 512 KB SRAM
- 384 KB ROM

### Flash

- 16 MB Quad SPI Flash
- Mapped at 0x00000000
- Used for firmware, NVS, SPIFFS, OTA partitions

### PSRAM

- 8 MB Octal PSRAM (critical distinction)
- MUST use `CONFIG_SPIRAM_MODE_OCT=y` (NOT Quad)
- Supports higher bandwidth for frame buffers and AI processing

## OV3660 Camera Sensor

| Specification | Value |
|---------------|-------|
| **Resolution** | 3 MP (1/5" sensor) |
| **Max Resolution** | QXGA 2048×1536 |
| **Sensor PID** | 0x77 (verified) |
| **Interface** | SCCB (I2C-like) |
| **Default XCLK** | 20 MHz |
| **Frame Format** | JPEG (`PIXFORMAT_JPEG`) |
| **Frame Buffers** | PSRAM-resident, count 2 |

### Frame Buffer Settings

- Location: PSRAM (`CAMERA_FB_IN_PSRAM`)
- Count: 2
- Format: JPEG
- Default resolution: VGA (640×480) when AI enabled

## Pin Map (GOOUUU board)

| Pin Name | GPIO | Notes |
|----------|------|-------|
| PWDN | -1 | Not connected |
| RESET | -1 | Not connected |
| XCLK | 15 | Camera clock output |
| SIOD | 4 | SCCB SDA |
| SIOC | 5 | SCCB SCL |
| D0 | 11 | Data bus bit 0 |
| D1 | 9 | Data bus bit 1 |
| D2 | 8 | Data bus bit 2 |
| D3 | 10 | Data bus bit 3 |
| D4 | 12 | Data bus bit 4 |
| D5 | 18 | Data bus bit 5 |
| D6 | 17 | Data bus bit 6 |
| D7 | 16 | Data bus bit 7 |
| VSYNC | 6 | Vertical sync |
| HREF | 7 | Horizontal reference |
| PCLK | 13 | Pixel clock |

**Note**: This is the GOOUUU pin mapping. Different N16R8 boards may have different pin assignments.

## Octal PSRAM Constraints

### Required Configuration

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y            # Octal, NOT Quad — R8 module
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y  # 64B cache line is MANDATORY for Octal DDR mode
```

### Critical Gotchas

1. **64B cache line is MANDATORY** - 32B causes silent data corruption
2. Octal PSRAM requires specific SDK configuration
3. Frame buffers MUST live in PSRAM (`CAMERA_FB_IN_PSRAM`)
4. AI grayscale buffers also allocated in PSRAM

## Flash Partition Plan

| Partition | Type | SubType | Offset | Size | Flags |
|-----------|------|---------|--------|------|-------|
| nvs | data | nvs | 0x9000 | 0x6000 (24 KB) | - |
| phy_init | data | phy | 0xf000 | 0x1000 (4 KB) | - |
| ota_0 | app | ota_0 | 0x10000 | 0x500000 (5 MB) | - |
| ota_1 | app | ota_1 | 0x510000 | 0x500000 (5 MB) | - |
| otadata | data | ota | 0xa10000 | 0x2000 (8 KB) | - |
| spiffs | data | spiffs | 0xa12000 | 0x80000 (512 KB) | - |

### Partition Usage

- **nvs**: Configuration storage (16 keys)
- **ota_0/ota_1**: Dual OTA slots for firmware updates
- **spiffs**: Web UI assets (HTML, CSS, JS)
- **Total used**: ~10.5 MB of 16 MB flash
- **Remaining**: ~5.5 MB available for future expansion

## Serial Port

### USB-Serial/JTAG

- Device: `/dev/ttyACM0` (Linux)
- Baudrate: 115200
- Data bits: 8
- Parity: None
- Stop bits: 1

### Permissions

User must be in appropriate group:
- Arch: `uucp`
- Debian/Ubuntu: `dialout`

### Finding the Port

```bash
ls /dev/serial/by-id/
# Or
ls /dev/ttyACM*
```

## Peripherals

### Flash LED

- GPIO candidates: 2, 3, 46 (probed at boot)
- Controlled via LED strip driver
- Brightness: 0-100 (PWM or GPIO-based)

### Status LED

- GPIO: Configured in `status_led.c`
- Indicates system state

## Power Requirements

- Input: 5V USB
- Current: ~500 mA typical (camera + WiFi + AI)
- PSRAM voltage: 1.1V internal
- Flash voltage: 3.3V

## Thermal Considerations

- Camera sensor can get warm during continuous operation
- AI processing increases CPU usage and heat
- Ensure adequate ventilation for long-term operation