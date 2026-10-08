# Web UI

## Overview

The MiBee Cam web UI is a responsive, browser-based interface for device control and monitoring. It features:

- zh/en internationalization
- Light/dark theme support
- Real-time video streaming with AI overlay
- Comprehensive settings panels
- Mobile-friendly responsive design

## Features

### Internationalization (i18n)

**Supported Languages**:
- English (en)
- Chinese (zh)

**Detection**:
- Auto-detects from `navigator.language` (zh → Chinese, others → English)
- Manual toggle in System section
- Persisted to `localStorage['mibee.lang']`

**Implementation**:
- 80+ translation keys
- Loaded via `/i18n.js` before app.js
- All UI elements use `data-i18n` attributes for translation

### Theme Support

**Modes**:
- Light mode
- Dark mode
- Auto (follows system preference)

**Detection**:
- Auto-detects from `prefers-color-scheme: dark` media query
- Manual toggle in System section
- Persisted to `localStorage['mibee.theme']`

**Implementation**:
- CSS custom properties for colors
- `data-theme="dark"` attribute on `<html>` element
- Smooth transitions between themes

## Settings Panels

### Camera Section

**Controls**:
- Resolution (framesize dropdown, 16 options from 96×96 to UXGA 1600×1200)
- JPEG Quality (slider, 0-63)
- Brightness (slider, -2 to +2)
- Contrast (slider, -2 to +2)
- Saturation (slider, -2 to +2)
- Sharpness (slider, -2 to +2)
- Horizontal Mirror (toggle)
- Vertical Flip (toggle)

**Behavior**:
- Sensor settings applied live with 300ms debounce
- Framesize/quality changes trigger camera reinit
- Resolution dropdown disables non-VGA options when AI enabled

**AI ↔ VGA Coupling**:
- Non-VGA framesize disables AI toggles
- AI enabled restricts framesize to VGA only (value 10)
- Visual feedback: disabled controls show 40% opacity

### AI Features Section

**Controls**:
- Face Detection (toggle)
- Motion Detection (toggle)
- QR Code (toggle)

**Behavior**:
- Changes applied immediately via POST /ai
- Persisted to config
- Live polling updates overlay every 500ms

**AI Overlay**:
- Green boxes for detected faces
- Motion score in top-left corner (orange)
- QR count in top-right corner (blue)
- Canvas overlay positioned over stream image

**Note**:
- AI requires VGA resolution (640×480)
- Displayed warning when AI enabled at non-VGA resolution

### Flash LED Section

**Controls**:
- LED Brightness (slider, 0-100)

**Behavior**:
- Applied live with 300ms debounce
- 0 = off, 100 = full brightness
- Controlled via GPIO to LED strip driver

### Network Section

**Controls**:
- WiFi SSID (text input)
- WiFi Password (password input)
- Save & Reboot button

**Behavior**:
- Save triggers device reboot
- 10-second countdown toast before expected disconnect
- Password can be left blank to keep current value

### Streaming Section

**Controls**:
- RTSP Username (text input)
- RTSP Password (password input)
- ONVIF Discovery (toggle)
- Save button

**Behavior**:
- RTSP credentials saved without reboot
- ONVIF toggle saved without reboot
- Changes applied to RTSP server immediately

### System Section

**Readouts**:
- Uptime (updated every 500ms)
- Free Heap (updated every 500ms)
- Free PSRAM (updated every 500ms)

**Controls**:
- Dark / Light toggle (theme)
- EN / 中文 toggle (language)

## Status Bar

The status bar at the top displays real-time device information:

- **WiFi**: Network connection status
- **IP**: Device IP address
- **Resolution**: Current camera resolution
- **Quality**: Current JPEG quality
- **Heap**: Free heap memory (KB)
- **PSRAM**: Free PSRAM (KB)
- **Clients**: Number of MJPEG stream clients
- **AI**: AI status (Active/Inactive)

**Updates**: Polling every 500ms via GET /status

## Stream Area

**Components**:
- Live MJPEG stream (`<img>` tag)
- Canvas overlay for AI detection results
- Connecting status message

**Reconnection**:
- Automatic reconnection on stream error
- Exponential backoff (1s → 30s max)
- Visual feedback during reconnection

**AI Overlay**:
- 640×480 canvas (scaled to match image)
- Face boxes: green rectangles
- Motion score: orange text at top-left
- QR codes: blue text at top-right
- Updated every 500ms via GET /ai/status

## Toast Notifications

**Uses**:
- Success messages (e.g., "Settings saved")
- Error messages (e.g., "AI failed: ...")
- Countdown timer before WiFi reboot

**Duration**:
- Default: 3 seconds
- Countdown: 1.2 seconds per tick

## Responsive Design

### Breakpoints

- **Mobile** (≤ 480px): Stacked layout, single column
- **Desktop** (> 480px): Side-by-side layout, settings panel on right

### Layout

**Mobile (< 480px)**:
```
+------------------+
|   Status Bar     |
+------------------+
|   Stream Area    |
|  (full width)    |
+------------------+
|  Settings Panel  |
|  (full width)    |
+------------------+
```

**Desktop (≥ 480px)**:
```
+------------------+------------------+
|   Status Bar     |
+------------------+------------------+
|                  |                  |
|   Stream Area    |  Settings Panel  |
|                  |                  |
+------------------+------------------+
```

## Accessibility

**Features**:
- Focus-visible rings on interactive elements
- ARIA switch roles for toggles
- Keyboard navigation (Enter/Space to activate toggles)
- Reduced motion support (respects `prefers-reduced-motion`)

## JavaScript Architecture

**Modules** (in app.js):

1. **Theme**: Theme management and persistence
2. **API Helper**: Simplified fetch wrapper with error handling
3. **Toast**: Notification system
4. **Toggle**: Reusable toggle component with keyboard support
5. **Tabs**: Tab navigation for settings panels
6. **Status Polling**: 500ms polling of /status
7. **AI Polling**: 500ms polling of /ai/status with canvas rendering
8. **Camera Panel**: Load/save camera settings with debouncing
9. **AI Coupling**: Enforce AI ↔ VGA relationship
10. **AI Toggle**: AI feature toggling via POST /ai
11. **LED Control**: Flash LED brightness via POST /led
12. **WiFi Save**: Network config via POST /config with reboot countdown
13. **Streaming Save**: RTSP/ONVIF config via POST /config
14. **Stream Reconnect**: Exponential backoff reconnection logic
15. **Init**: DOMContentLoaded initialization

## Browser Compatibility

**Minimum Requirements**:
- ES6 JavaScript support
- Fetch API
- CSS Custom Properties
- Canvas API
- LocalStorage API

**Tested Browsers**:
- Chrome 90+
- Firefox 88+
- Safari 14+
- Edge 90+

## File Structure

```
main/web_ui/
├── index.html    # Main HTML structure
├── style.css     # Styles and responsive design
├── i18n.js       # Internationalization dictionary
└── app.js        # Application logic
```

**Served from SPIFFS**:
- All files mounted at `/spiffs/`
- Served via GET / static file handler
- Embedded in firmware via `spiffs_create_partition_image`