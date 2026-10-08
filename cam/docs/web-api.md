# Web API

## Overview

The MiBee Cam exposes a REST API on port 80 for device control and monitoring. All endpoints return JSON responses with CORS headers enabled.

## Base URL

```
http://<device-ip>/
```

## Endpoints

### 1. GET / — Static File Serving

Serves the web UI from SPIFFS.

**Content-Type Mapping**:
- `.html` → `text/html`
- `.css` → `text/css`
- `.js` → `application/javascript`
- `.png` → `image/png`
- `.jpg` / `.jpeg` → `image/jpeg`
- `.ico` → `image/x-icon`
- `.svg` → `image/svg+xml`
- `.json` → `application/json`

**Files Served**:
- `/index.html` → Main web UI
- `/style.css` → Styles
- `/app.js` → Application logic
- `/i18n.js` → Internationalization

### 2. GET /stream — MJPEG Live Stream

Returns a multipart HTTP response with JPEG frames.

**Response**:
```
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg
Content-Length: <size>

<JPEG data>
--frame
...
```

**Client Behavior**:
- Auto-reconnect with exponential backoff (1s → 30s max)
- Handle disconnection gracefully

### 3. GET /status — Device Status

Returns current device status including WiFi, camera, AI, and system metrics.

**Response**:
```json
{
  "ok": true,
  "data": {
    "wifi_ssid": "MyNetwork",
    "ip": "192.168.1.100",
    "camera_resolution": "VGA 640x480",
    "camera_framesize": 10,
    "camera_quality": 12,
    "cam_brightness": 0,
    "cam_contrast": 0,
    "cam_saturation": 0,
    "cam_sharpness": 0,
    "cam_hmirror": false,
    "cam_vflip": false,
    "free_heap": 245760,
    "free_psram": 8388608,
    "mjpeg_clients": 2,
    "ai_status": {
      "face": true,
      "motion": false,
      "qr": true
    }
  }
}
```

**Note**: `ai_status` uses live `ai_is_enabled()` calls, not just config values.

### 4. GET /config — Current Configuration

Returns the full configuration with passwords masked.

**Response**:
```json
{
  "ok": true,
  "data": {
    "wifi_ssid": "MyNetwork",
    "wifi_pass": "****",
    "cam_framesize": 10,
    "cam_quality": 12,
    "ai_face_enable": true,
    "ai_motion_enable": false,
    "ai_qr_enable": true,
    "rtsp_user": "admin",
    "rtsp_pass": "****",
    "onvif_enable": true,
    "cam_brightness": 0,
    "cam_contrast": 0,
    "cam_saturation": 0,
    "cam_sharpness": 0,
    "cam_hmirror": false,
    "cam_vflip": false,
    "mjpeg_clients": 2
  }
}
```

### 5. POST /config — Update Configuration

Update configuration values. WiFi changes trigger a device reboot.

**Request Body** (JSON, any subset of keys):
```json
{
  "wifi_ssid": "NewNetwork",
  "wifi_pass": "newpassword",
  "cam_framesize": 10,
  "cam_quality": 12,
  "ai_face_enable": true,
  "ai_motion_enable": false,
  "ai_qr_enable": true,
  "rtsp_user": "admin",
  "rtsp_pass": "password",
  "onvif_enable": true,
  "cam_brightness": 0,
  "cam_contrast": 0,
  "cam_saturation": 0,
  "cam_sharpness": 0,
  "cam_hmirror": false,
  "cam_vflip": false
}
```

**Special Values**:
- `"****"` for passwords = unchanged (do not update)

**Response**:
```json
{
  "ok": true,
  "data": {
    "updated": 3
  }
}
```

**WiFi Behavior**:
- WiFi changes are saved and device reboots to apply
- No live reconnection (requires full reboot)

### 6. GET /camera — Camera Configuration

Returns current camera settings.

**Response**:
```json
{
  "ok": true,
  "data": {
    "cam_framesize": 10,
    "cam_quality": 12,
    "cam_brightness": 0,
    "cam_contrast": 0,
    "cam_saturation": 0,
    "cam_sharpness": 0,
    "cam_hmirror": false,
    "cam_vflip": false,
    "cam_framesize_name": "VGA 640x480"
  }
}
```

**Framesize Values**:
- 0: 96×96
- 1: QQVGA 160×120
- 2: 128×128
- 3: QCIF 176×144
- 4: HQVGA 240×176
- 5: 240×240
- 6: QVGA 320×240
- 7: 320×320
- 8: CIF 400×296
- 9: HVGA 480×320
- **10: VGA 640×480** (default, required for AI)
- 11: SVGA 800×600
- 12: XGA 1024×768
- 13: HD 1280×720
- 14: SXGA 1280×1024
- 15: UXGA 1600×1200

### 7. POST /camera — Update Camera Settings

Update camera configuration. Sensor settings applied live; framesize/quality require coordinated reinit.

**Request Body** (JSON, any subset of keys):
```json
{
  "cam_framesize": 10,
  "cam_quality": 12,
  "cam_brightness": 0,
  "cam_contrast": 0,
  "cam_saturation": 0,
  "cam_sharpness": 0,
  "cam_hmirror": false,
  "cam_vflip": false
}
```

**Validation**:
- `cam_framesize`: 0-24
- `cam_quality`: 0-63
- Sensor settings: -2 to +2

**AI Safety Check**:
- Rejects non-VGA framesize when any AI feature is enabled
- Returns HTTP 400 with message: "Disable AI to use non-VGA resolution"

**Behavior**:
- Sensor settings (brightness/contrast/saturation/sharpness/mirror/flip): Applied live via sensor registers
- Framesize/quality: Triggers coordinated camera reinit (stop broadcaster+AI → deinit → init → restart)
- Response: Returns current camera state (same as GET /camera)

### 8. POST /ai — Toggle AI Features

Enable or disable AI features. Changes are applied live AND persisted.

**Request Body**:
```json
{
  "face": true,
  "motion": false,
  "qr": true
}
```

**Response**:
```json
{
  "ok": true,
  "data": {
    "updated": 2,
    "face": true,
    "motion": false,
    "qr": true
  }
}
```

**Behavior**:
- Calls `ai_enable()` LIVE (not just persist)
- Saves to config
- Returns current AI state

### 9. GET /ai/status — AI Detection Results

Returns latest AI detection results. Returns 404 if AI pipeline not running.

**Response**:
```json
{
  "ok": true,
  "data": {
    "face": {
      "count": 2,
      "boxes": [
        {
          "x": 100,
          "y": 150,
          "w": 80,
          "h": 100,
          "confidence": 0.95
        },
        {
          "x": 300,
          "y": 200,
          "w": 70,
          "h": 90,
          "confidence": 0.88
        }
      ]
    },
    "motion": {
      "score": 23
    },
    "qr": {
      "count": 1,
      "codes": [
        "https://example.com"
      ]
    },
    "seq": 12345
  }
}
```

**Response Codes**:
- 200 OK: AI running, results available
- 404 Not Found: AI pipeline not running

### 10. POST /led — Flash LED Control

Set flash LED brightness.

**Request Body**:
```json
{
  "brightness": 50
}
```

**Validation**:
- `brightness`: 0-100 (0 = off, 100 = full)

**Response**:
```json
{
  "ok": true,
  "data": {
    "brightness": 50
  }
}
```

### 11. OPTIONS /\* — CORS Preflight

Handles CORS preflight requests for cross-origin requests.

**Response**:
```
HTTP/1.1 200 OK
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## Error Responses

All error responses follow this format:

```json
{
  "ok": false,
  "error": "Error message here"
}
```

**Common HTTP Status Codes**:
- 200 OK: Success
- 400 Bad Request: Invalid input or validation failure
- 404 Not Found: Resource not available (e.g., AI not running)
- 500 Internal Server Error: Server-side error

## CORS

All endpoints include CORS headers:

```
Access-Control-Allow-Origin: *
```

## Polling Intervals

Recommended polling intervals for clients:
- `/status`: 500ms
- `/ai/status`: 500ms (only when AI enabled)
- `/camera`: On-demand (user interaction)
- `/config`: On-demand (user interaction)