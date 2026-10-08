/*
 * MiBee Cam v0.1 — WiFi AP/STA dual-mode manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Manages WiFi in STA mode (with NVS credentials from config_manager)
 * or falls back to AP mode ("MiBeeCam-XXXX") when STA fails or no
 * credentials are available.
 *
 * Call wifi_manager_init() once after config_load() in the boot sequence.
 * It returns immediately — connection status is tracked via accessors.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise WiFi in STA or AP mode.
 *
 *         - If WiFi credentials are present in NVS -> STA mode.
 *         - On STA connection -> green status LED.
 *         - On STA failure (15 s timeout or 3 disconnects) -> AP fallback.
 *         - If no credentials -> AP mode immediately.
 *         - AP mode -> blue status LED, SSID "MiBeeCam-XXXX".
 *
 * @note   Non-blocking — does NOT wait for connection.
 *         Call after nvs_flash_init() + config_load().
 */
esp_err_t wifi_manager_init(void);

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  True when STA is connected and has an IP address.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief  Return the current IP address as a string.
 *         "0.0.0.0" if not connected.  Static buffer — valid until next
 *         network event.
 */
const char *wifi_manager_get_ip(void);

/* 双网络（2026-09-04）：当前激活网络与实际 SSID（"primary"/"secondary"/"ap"） */
const char *wifi_manager_active_net(void);
const char *wifi_manager_current_ssid(void);

/* 2026-10-08 连接兼容性阶梯：当前 STA 射频/关联档位名
 * （"p0-default"/"p1-steer"/"p2-ht20"/"p3-legacy"/"p4-randmac"/"p5-wpa2"，
 * 整轮失败逐级降级，p4 换每 SSID 派生 MAC，p5 关 PMF 强制 WPA2） */
const char *wifi_manager_compat_profile(void);

/* 2026-09-04 API 对齐：三姐妹板的 /api/status 都带 wifi_rssi/wifi_channel
 * （SPA 信号芯片 + WiFi 页当前连接行依赖）。未连接返回 0。 */
int wifi_manager_get_rssi(void);
int wifi_manager_get_channel(void);

#ifdef __cplusplus
}
#endif
