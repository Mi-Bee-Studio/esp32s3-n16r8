/*
 * MiBee Cam v0.1 — WiFi AP/STA dual-mode manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wifi_manager.h"
#include "config_manager.h"
#include "status_led.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi_manager";

/* ---- module state ------------------------------------------------- */

static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t       *s_netif_sta   = NULL;
static esp_netif_t       *s_netif_ap    = NULL;

static int   s_sta_retry_count = 0;
static bool  s_ap_started      = false;   /* guard against double AP fallback */
static bool  s_sta_connected   = false;
static char  s_ip_str[16]      = "0.0.0.0";

/* ---- 双网络故障转移（2026-09-04，此前本板单 WiFi：主网弱态即失联） ---- */
static bool s_using_secondary = false;    /* 当前激活的凭据组 */
static int  s_net_switches    = 0;        /* 本轮开机的网络切换次数（防乒乓） */
static char s_current_ssid[33] = "";      /* 当前实际连接的 SSID */
static TickType_t s_ip_granted_tick = 0;   /* 最近一次 GOT_IP 时刻 */
static bool s_link_flap = false;          /* 连接存活 <60s：切换预算不清零（2026-09-29 unit-2 教训：GOT_IP 即清零让 NET_MAX_SWITCHES 在"连上又被踢"风暴里形同虚设） */
#define LINK_STABLE_MS     (60 * 1000)    /* 连接存活超过此值才算"稳" */
#define NET_FAILS_SWITCH   2              /* 当前网连续失败 N 次后切备用 */
#define NET_MAX_SWITCHES   6              /* 超过则放弃转 AP */
#define DHCP_TIMEOUT_MS    12000          /* 关联后无 IP 判 DHCP 盲区（PIT：ai-thinker 教训） */

static bool secondary_configured(void)
{
    const char *ssid2 = config_get_wifi_ssid_2();
    return ssid2 && ssid2[0];
}

static void save_last_net(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi_pref", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "last_net", s_using_secondary ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_last_net(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("wifi_pref", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "last_net", &v);
        nvs_close(h);
    }
    return v == 1;
}

/* event-group bits */
#define CONNECTED_BIT   BIT0

/* ---- constants ---------------------------------------------------- */
#define STA_MAX_RETRIES     3
#define STA_CONNECT_TIMEOUT_MS  15000   /* 15 s before AP fallback */
#define AP_CHANNEL          6
#define AP_MAX_CONNECTIONS  4
#define AP_STA_RETRY_MS     (30 * 60 * 1000)  /* AP 兜底后周期重试间隔（PIT-060） */

/* ---- forward declarations ---------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data);
static void sta_apply_and_connect(bool secondary);
static bool failover_to_other_net(const char *why);
static void get_ap_ssid(char *buf, size_t len);
static void start_ap(void);
static void switch_to_ap(void);
static bool ap_fallback(void);   /* AP 兜底闸门（allow_ap_fallback，契约 §3.1） */
static void connection_monitor_task(void *arg);
static void start_sta(void);
static void ap_sta_retry_task(void *arg);

/* ------------------------------------------------------------------ */
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief  Build AP SSID from the last 2 bytes of the softAP MAC.
 *         Format: "MiBeeCam-XXXX"
 */
static void get_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "MiBeeCam-%02X%02X", mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/*  event handler                                                      */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: {
            /* 双网都已配置时开机择优：快扫一次，信号强 ≥8dB 者胜出；
             * 否则沿用上次好网。.119 板位主网"弱而不断"，纯失败转移
             * 永远不会触发，必须在这里比 RSSI。扫描 ~1-2s，仅开机一次。 */
            const char *p1 = config_get_wifi_ssid();
            const char *p2 = config_get_wifi_ssid_2();
            if (p1[0] && p2[0] && strcmp(p1, p2) != 0) {
                wifi_scan_config_t sc = { 0 };
                sc.show_hidden = false;
                if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                    uint16_t n = 0;
                    esp_wifi_scan_get_ap_num(&n);
                    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * (n ? n : 1));
                    if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                        int8_t r1 = -128, r2 = -128;
                        for (int i = 0; i < n; i++) {
                            if (strcmp((const char *)recs[i].ssid, p1) == 0 && recs[i].rssi > r1) r1 = recs[i].rssi;
                            if (strcmp((const char *)recs[i].ssid, p2) == 0 && recs[i].rssi > r2) r2 = recs[i].rssi;
                        }
                        free(recs);
                        bool want2 = s_using_secondary;
                        if (r1 > -128 && r2 > -128) {
                            if (r2 - r1 >= 8)       want2 = true;
                            else if (r1 - r2 >= 8)  want2 = false;
                            /* 差距 <8dB：保持 last_net */
                            ESP_LOGI(TAG, "boot pick: '%s' %ddBm vs '%s' %ddBm → %s",
                                     p1, r1, p2, r2, want2 ? "secondary" : "primary");
                        } else if (r2 > -128 && r1 == -128) {
                            want2 = true;   /* 主网不在空中 */
                        }
                        s_using_secondary = want2;
                    } else { free(recs); }
                }
            }
            sta_apply_and_connect(s_using_secondary);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc = event_data;
            s_sta_connected = false;
            xEventGroupClearBits(s_event_group, CONNECTED_BIT);
            s_sta_retry_count++;
            /* reason 码入日志（2026-09-29 unit-2 排障教训：无 reason 的
             * disconnect 无法区分 AP 踢人/认证失败/信标丢失） */
            ESP_LOGW(TAG, "STA disconnected from '%s' (fail %d, reason=%d)",
                     s_current_ssid, s_sta_retry_count,
                     disc ? disc->reason : -1);
            /* 短命连接标记：防"连上即清零"打穿切换预算 */
            if (s_ip_granted_tick > 0 &&
                (xTaskGetTickCount() - s_ip_granted_tick) * portTICK_PERIOD_MS < LINK_STABLE_MS) {
                s_link_flap = true;
            } else if (s_ip_granted_tick > 0) {
                s_link_flap = false;   /* 稳过 60s 的连接：预算可恢复 */
            }
            status_led_set_color(STATUS_LED_RED);

            /* 主网连续失败 → 先试备用网，再谈 AP 兜底 */
            if (s_sta_retry_count >= NET_FAILS_SWITCH &&
                failover_to_other_net("connect failures")) {
                break;
            }
            if (s_sta_retry_count >= STA_MAX_RETRIES) {
                ESP_LOGW(TAG, "STA max retries reached");
                if (ap_fallback()) {
                    break;
                }
                /* 兜底被禁：分批继续重试（清零计数避免每事件刷 WARN） */
                s_sta_retry_count = 0;
            }
            esp_wifi_connect();
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP mode started");
            status_led_set_color(STATUS_LED_BLUE);
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str),
                     IPSTR, IP2STR(&evt->ip_info.ip));
            s_sta_retry_count = 0;
            s_ip_granted_tick = xTaskGetTickCount();
            if (!s_link_flap) {
                s_net_switches = 0;     /* 稳定连接才恢复转移预算；短命
                                         * 连接不清零，防震荡打穿上限 */
            }
            s_sta_connected   = true;
            save_last_net();
            xEventGroupSetBits(s_event_group, CONNECTED_BIT);
            status_led_set_color(STATUS_LED_GREEN);
            ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR,
                     IP2STR(&evt->ip_info.ip));
        }
    }
}

/* ------------------------------------------------------------------ */
/*  STA credential switching                                           */
/* ------------------------------------------------------------------ */

/** Apply the given network's credentials to the running STA and connect. */
static void sta_apply_and_connect(bool secondary)
{
    s_using_secondary = secondary;
    const char *ssid = secondary ? config_get_wifi_ssid_2() : config_get_wifi_ssid();
    const char *pass = secondary ? config_get_wifi_pass_2() : config_get_wifi_pass();

    wifi_config_t sta_config = { 0 };
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* auto-negotiate: OPEN~WPA3 */
    sta_config.sta.pmf_cfg.capable    = true;
    sta_config.sta.pmf_cfg.required   = false;
    sta_config.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
    sta_config.sta.listen_interval    = 3;

    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    /* 2026-09-06（PIT-034 试点）：ESPectre CSI 策略应用会异步重连 STA，
     * 断连事件驱动的重连可能与该窗口竞态，set_config 返回状态错误——
     * ESP_ERROR_CHECK 会直接 abort 重启（实测 line 226 一次），改温和处理。 */
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config %s (radio reconfig in flight?) — retry next event",
                 esp_err_to_name(cfg_err));
        return;
    }
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA connecting to [%s]: %s",
             secondary ? "secondary" : "primary", ssid);
}

/** Switch to the other network if configured; returns true if switched. */
static bool failover_to_other_net(const char *why)
{
    if (s_ap_started) return false;
    if (s_net_switches >= NET_MAX_SWITCHES) {
        ESP_LOGW(TAG, "net switch cap reached (%d)", s_net_switches);
        if (ap_fallback()) {
            return false;
        }
        /* 兜底被禁：清零切换计数，继续在两网间轮换重试 */
        s_net_switches = 0;
        return false;
    }
    bool target = !s_using_secondary;
    if (target && !secondary_configured()) return false;   /* nowhere to go */
    s_net_switches++;
    s_sta_retry_count = 0;
    ESP_LOGW(TAG, "%s — switching to %s network (switch %d/%d)",
             why, target ? "secondary" : "primary", s_net_switches, NET_MAX_SWITCHES);
    sta_apply_and_connect(target);
    return true;
}

/* ------------------------------------------------------------------ */
/*  AP mode                                                            */
/* ------------------------------------------------------------------ */

/** Start fresh AP mode (no prior STA netif). */
static void start_ap(void)
{
    s_ap_started  = true;             /* mark AP started to prevent double-fallback */
    s_netif_ap = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = { 0 };
    get_ap_ssid((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.channel         = AP_CHANNEL;
    ap_config.ap.max_connection  = AP_MAX_CONNECTIONS;
    /* 2026-09-05 轮换：开放 AP → WPA2，家族统一公开入门密码（mibeecam2026） */
    ap_config.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap_config.ap.password, "mibeecam2026", sizeof(ap_config.ap.password));
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* static IP for AP: 192.168.4.1/24 */
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_netif_ap);
    esp_netif_set_ip_info(s_netif_ap, &ip_info);
    esp_netif_dhcps_start(s_netif_ap);

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    s_sta_connected = false;   /* not in STA mode */

    ESP_LOGI(TAG, "AP started, SSID: %s, IP: 192.168.4.1",
             ap_config.ap.ssid);
}

/**
 * @brief  Transition from STA to AP: stop WiFi, tear down STA netif,
 *         start AP.
 */
static void switch_to_ap(void)
{
    if (s_ap_started) {
        ESP_LOGD(TAG, "AP already started, ignoring duplicate switch_to_ap");
        return;
    }
    s_ap_started = true;

    esp_wifi_stop();

    if (s_netif_sta) {
        esp_netif_destroy(s_netif_sta);
        s_netif_sta = NULL;
    }

    start_ap();

    /* PIT-060（2026-10-01 二号机复发 37h 实锤）：带凭据进 AP 兜底 → 武装
     * 周期 STA 重试，路由器侧恢复收容后 ≤一个间隔自愈，免人工 AT+REBOOT。
     * 无凭据是首启 provision（配置保存即重启），不重试。 */
    if (config_get_wifi_ssid()[0]) {
        xTaskCreate(ap_sta_retry_task, "ap_retry", 3072, NULL, 4, NULL);
    }
}

/**
 * @brief  AP 兜底后的周期 STA 重试（PIT-060：原实现 AP 回退即永久放弃，
 *         路由器间歇拒收场景把板子钉死热点模式）。
 *
 *         每 AP_STA_RETRY_MS 切回 STA 试一轮——AP→STA 逆 switch_to_ap，
 *         连接全量复用启动路径（STA_START 择优扫描 + connection_monitor
 *         两段式 DHCP 等待）。成败由 monitor 裁决：成 → 常驻 STA；败 →
 *         ap_fallback() 回 AP 并重新武装本任务，无限慢速循环。
 *         不订阅 TWDT（30min 单睡合法，PIT-059 纪律只约束已订阅任务）。
 *         AP 上有配置客户端时顺延一轮，不打断 provision。
 */
static void ap_sta_retry_task(void *arg)
{
    int attempt = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AP_STA_RETRY_MS));
        if (!s_ap_started) break;               /* 已被其它路径切回 STA */
        if (!config_get_wifi_ssid()[0]) break;  /* 无凭据：provision 态不重试 */
        wifi_sta_list_t stas;
        if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK && stas.num > 0) {
            ESP_LOGI(TAG, "AP retry postponed: %d station(s) on config portal", stas.num);
            continue;
        }
        ESP_LOGW(TAG, "AP fallback periodic STA retry (attempt %d) — leaving AP mode",
                 ++attempt);
        esp_wifi_stop();
        if (s_netif_ap) {
            esp_netif_destroy(s_netif_ap);
            s_netif_ap = NULL;
        }
        s_ap_started      = false;
        s_net_switches    = 0;   /* 本轮重试给满切换预算 */
        s_sta_retry_count = 0;
        s_link_flap       = false;
        start_sta();
        TaskHandle_t mon = NULL;
        xTaskCreate(connection_monitor_task, "wifi_mon", 2048, NULL, 5, &mon);
        (void)mon;
        break;   /* 交付 monitor：成→STA 常驻；败→ap_fallback 重新武装 */
    }
    vTaskDelete(NULL);
}

/** AP 兜底闸门（契约 §3.1 allow_ap_fallback，NVS 键 ap_fallback，默认 1
 *  = 保留现行为）。返回 true = 已进入 AP；false = 兜底被禁，调用方按
 *  各自节奏继续 STA 重试。无凭据时的首次 start_ap()（provisioning）
 *  不受此闸门约束。 */
static bool ap_fallback(void)
{
    if (!config_get_allow_ap_fallback()) {
        ESP_LOGW(TAG, "AP fallback disabled (allow_ap_fallback=0) — keep retrying STA");
        return false;
    }
    switch_to_ap();
    return true;
}

/* ------------------------------------------------------------------ */
/*  STA mode                                                           */
/* ------------------------------------------------------------------ */

static void start_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 上次拿到 IP 的网络优先（ai-thinker 实测 40s→3.3s 的同款修复） */
    s_using_secondary = load_last_net() && secondary_configured();

    const char *ssid = s_using_secondary ? config_get_wifi_ssid_2()
                                         : config_get_wifi_ssid();
    strlcpy(s_current_ssid, ssid ? ssid : "", sizeof(s_current_ssid));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* 射频调优（2026-09-13 实测驱动，AGENTS 2026-09-04 挂账候选落地）：
     * 静默基线 ping 10-33% 丢包、RTT 均值 177ms/峰值 525ms——RSSI -40dBm
     * 强信号下唯一合理解释是射频睡眠 + HT40 邻道干扰。
     * ① PS_NONE：IDF 默认 MIN_MODEM 让射频在 DTIM 间休眠，对浏览器页
     *    加载/预览这类延迟敏感流量就是延迟尖峰+丢包；本板 USB 供电，
     *    流媒体设备不省这点电（家族 PIT-001 备注"出厂应 PS_NONE"，
     *    本仓此前漏设）。
     * ② HT20：2.4G HT40 要占第二个 20MHz 信道，ch7 环境给不起；
     *    HT20 72Mbps PHY 远超推流需求。AMPDU 维持关闭不动（PIT 复测结论）。 */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    /* BW 不强制：2026-09-13 A/B 实验——HT20 强制后双客户端拉流下 ping
     * 52.5% 丢包/RTT 173ms（每帧占双倍空口时间，负载态更糟），回退
     * 跟随 AP 协商；PS_NONE 保留（延迟尖峰根因）。挪信道是 AP 侧动作。 */
    ESP_LOGI(TAG, "radio tune: PS_NONE ret=%s", esp_err_to_name(ps_err));

    /* STA_START 事件里统一走 sta_apply_and_connect（凭据 + 连接） */
    status_led_set_color(STATUS_LED_RED);
}

/* ------------------------------------------------------------------ */
/*  connection monitor (background)                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief  Task that waits up to 15 s for the STA connection.
 *         If it times out, falls back to AP mode.
 *         Self-deletes after completing.
 */
static void connection_monitor_task(void *arg)
{
    /* 两段式：先给当前网 DHCP_TIMEOUT_MS 拿 IP；关联得上但拿不到 IP 是
     * DHCP 盲区（主网弱态的典型症状），直接切网而不是干等。 */
    for (int stage = 0; stage < 2; stage++) {
        EventBits_t bits = xEventGroupWaitBits(
            s_event_group, CONNECTED_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(DHCP_TIMEOUT_MS));
        if (bits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "STA connection established (stage %d)", stage);
            vTaskDelete(NULL);
            return;
        }
        if (stage == 0 && failover_to_other_net("no IP (DHCP blind spot)")) {
            continue;   /* second window for the other network */
        }
        break;
    }
    ESP_LOGW(TAG, "no IP on either network");
    ap_fallback();   /* 兜底被禁时仅告警；STA 断线事件的重连循环继续 */
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t wifi_manager_init(void)
{
    /* ---- one-time netif + event infrastructure -------------------- */
    esp_netif_init();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        return ESP_ERR_NO_MEM;
    }

    /* one-time WiFi stack init */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* register for all WiFi / IP events — we filter by id in the handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    /* ---- decide mode --------------------------------------------- */
    const char *ssid = config_get_wifi_ssid();

    if (ssid && strlen(ssid) > 0) {
        start_sta();

        /* background monitor: 15 s timeout → AP fallback */
        TaskHandle_t monitor_task = NULL;
        xTaskCreate(connection_monitor_task, "wifi_mon",
                    2048, NULL, 5, &monitor_task);
        (void)monitor_task;
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting AP mode");
        start_ap();
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_sta_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}

const char *wifi_manager_active_net(void)
{
    if (s_ap_started) return "ap";
    return s_using_secondary ? "secondary" : "primary";
}

const char *wifi_manager_current_ssid(void)
{
    return s_current_ssid[0] ? s_current_ssid : config_get_wifi_ssid();
}

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

int wifi_manager_get_channel(void)
{
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.primary;
    }
    return 0;
}
