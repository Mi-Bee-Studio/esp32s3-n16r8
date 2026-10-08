/*
 * at_port.c — esp32s3-n16r8 板级 port 层（家族 AT 核心 at_command.c 的钩子）
 *
 * IO：UART0（CH340，/dev/ttyUSB1）115200-8N1，UART 驱动 + VFS + fgets
 * （承袭本仓旧 at_command.c 的成熟路径：驱动已装则不重装、上电静默期
 * 由核心处理）。生效语义（契约 §6）：分辨率/画质热重配（camera_reinit
 * 协调停 AI/广播再重建）；WiFi 保存+1s 重启。
 *
 * 本仓 config 写路径为 config_set()（仅内存）+ config_save()（落盘）——
 * 与 ai-thinker 的"setter 自带落盘"不同，故本 port 每个 setter 显式落盘，
 * AT+SAVE 为等价幂等操作。
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "at_port.h"
#include "config_manager.h"
#include "csi_motion.h"   /* 契约 v1.7：CSI 调参键族热应用 + AT+CSI 快照 */
#include "wifi_channel_health.h"   /* 契约 v1.7 ①b：信道健康快照 */
#include "camera_driver.h"
#include "wifi_manager.h"
#include "ai_pipeline.h"
#include "flash_led.h"

/* ── 小工具 ────────────────────────────────────────────────────── */

/* 严格整数解析：全串必须是数字（含符号），否则失败 */
static bool parse_long_strict(const char *s, long *out)
{
    if (!s || !s[0]) {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* "on"/"off"/"1"/"0"/"true"/"false"（大小写不敏感）→ bool（承袭旧 AT） */
static bool parse_onoff(const char *s, bool *out)
{
    if (!s || !s[0]) return false;
    if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* config_set（内存）+ config_save（落盘）：本仓写路径标准二连 */
static esp_err_t cfg_write(const char *key, const char *val)
{
    esp_err_t ret = config_set(key, val);
    if (ret != ESP_OK) {
        return ret;
    }
    return config_save();
}

/* ── 能力裁剪 ──────────────────────────────────────────────────── */

static const at_caps_t s_caps = {
    .wifi_scan = true,
    .cfg       = true,
};

const at_caps_t *at_port_caps(void)
{
    return &s_caps;
}

/* ── IO：UART0 + VFS（fgets 阻塞行读） ─────────────────────────── */

esp_err_t at_port_init(void)
{
    /* 控制台可能已装驱动（共口 ESP_LOG）：已装则不重装 */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0);
    }
    /* VFS 从 ROM（只出）切到 UART 驱动（双向），stdin 方可 fgets */
    uart_vfs_dev_use_driver(UART_NUM_0);
    return ESP_OK;
}

int at_port_getline(char *buf, int len)
{
    if (!fgets(buf, len, stdin)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return -1;
    }
    /* 去 \r\n */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
    fflush(stdout);
    return (int)n;
}

void at_port_write(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

/* ── GMR / 传感器 ──────────────────────────────────────────────── */

void at_port_gmr_info(at_gmr_info_t *out)
{
    strlcpy(out->board, "esp32s3-n16r8", sizeof(out->board));
    /* 实测型号（信设备不信文档）：查 esp32-camera 组件能力表 */
    strlcpy(out->sensor, camera_sensor_name(), sizeof(out->sensor));
}

/* ── WiFi ──────────────────────────────────────────────────────── */

void at_port_wifi_info(at_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *net = wifi_manager_active_net();   /* primary/secondary/ap */
    if (strcmp(net, "ap") == 0) {
        strlcpy(out->state, "ap", sizeof(out->state));
        strlcpy(out->ssid, "", sizeof(out->ssid));
    } else if (wifi_manager_is_connected()) {
        strlcpy(out->state, "connected", sizeof(out->state));
        /* 实际连接的 SSID（区别于配置值） */
        strlcpy(out->ssid, wifi_manager_current_ssid(), sizeof(out->ssid));
    } else {
        /* 本板 wifi_manager 未暴露独立的 connecting 态 */
        strlcpy(out->state, "disconnected", sizeof(out->state));
        strlcpy(out->ssid, wifi_manager_current_ssid(), sizeof(out->ssid));
    }
    strlcpy(out->ip, wifi_manager_get_ip(), sizeof(out->ip));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(out->gw, sizeof(out->gw), IPSTR, IP2STR(&ip.gw));
            snprintf(out->mask, sizeof(out->mask), IPSTR, IP2STR(&ip.netmask));
        }
    }
}

esp_err_t at_port_wifi_set(const char *ssid, const char *pass)
{
    /* 本板语义（契约 §6）：保存 + 重启（凭据只在启动/故障切换时读取） */
    esp_err_t ret = cfg_write("wifi_ssid", ssid);
    if (ret == ESP_OK) {
        ret = cfg_write("wifi_pass", pass);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    at_port_write("+REBOOTING: wifi credentials saved\r\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

esp_err_t at_port_wifi_scan(at_scan_emit_fn emit)
{
    /* 承袭旧 AT+WIFISCAN：阻塞扫描，上限 16 条，RSSI 降序 */
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return ESP_OK;   /* 核心回 OK（无 AP 在视野内不是错误） */
    }
    if (n > 16) n = 16;

    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * n);
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    /* RSSI 降序（契约 §2 与 /api/scan 一致） */
    for (int i = 1; i < (int)n; i++) {
        wifi_ap_record_t key = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }
    for (int i = 0; i < (int)n; i++) {
        emit((const char *)recs[i].ssid, recs[i].rssi, recs[i].authmode);
    }
    free(recs);
    return ESP_OK;
}

/* ── 摄像头 ────────────────────────────────────────────────────── */

/* 长标签表：与 GET /api/camera 的 supported_resolutions 同源（web_server.c
 * 的 res_labels 数组），按 10..effective_max 循环生成 */
bool at_port_cam_res_info(at_cam_res_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cur = config_get_cam_framesize();
    out->cur_label = camera_framesize_name((uint8_t)out->cur);

    static const char *res_labels[] = {
        [10] "VGA (640x480)",  [11] "SVGA (800x600)",
        [12] "XGA (1024x768)", [13] "HD (1280x720)",
        [14] "SXGA (1280x1024)", [15] "UXGA (1600x1200)",
    };
    static at_res_opt_t s_opts[6];
    int eff_max = camera_get_effective_max_res();
    int n = 0;
    for (int fs = 10; fs <= eff_max && fs < (int)(sizeof(res_labels) / sizeof(res_labels[0])); fs++) {
        if (!res_labels[fs]) continue;
        if (n >= (int)(sizeof(s_opts) / sizeof(s_opts[0]))) break;
        s_opts[n].value = fs;
        s_opts[n].label = res_labels[fs];
        n++;
    }
    out->opts = s_opts;
    out->opt_count = n;

    out->cap = eff_max;
    out->cap_source = camera_res_cap_source();
    return true;
}

esp_err_t at_port_cam_res_set(int value)
{
    /* 合法性 = 在板级可选表内 且 ≤ 三层上限（与 POST /api/camera 同源） */
    at_cam_res_info_t r;
    if (!at_port_cam_res_info(&r)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = false;
    for (int i = 0; i < r.opt_count; i++) {
        if (r.opts[i].value == value) { ok = true; break; }
    }
    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    /* AI 管线硬编码 VGA（640x480 缓冲）：非 VGA + 任一 AI 开启 → 拒绝
     * （承袭旧 AT+CAMRES；AI 会话由协调重配停止属静默副作用，不如拒绝直观）。
     * 核心对 INVALID_ARG 只报 unsupported(max)，故此处点名真实原因。 */
    if (!camera_framesize_is_vga((uint8_t)value) &&
        (ai_is_enabled(AI_FEATURE_FACE_DETECT) ||
         ai_is_enabled(AI_FEATURE_MOTION_DETECT) ||
         ai_is_enabled(AI_FEATURE_QR_DECODE))) {
        at_port_write("ERROR: disable AI to use non-VGA resolution (AIFACE/AIMOTION/AIQR=off)\r\n");
        return ESP_ERR_INVALID_ARG;
    }
    /* 本板生效语义（契约 §6）：热重配（camera_reinit 内部停 AI/广播 →
     * 重建 → 持久化新档；失败自动回滚旧档，回滚失败才重启自愈） */
    esp_err_t ret = camera_reinit((uint8_t)value, config_get_cam_quality());
    if (ret != ESP_OK) {
        return ret;
    }
    at_port_write("+APPLY: hot-reconfig done\r\n");
    return ESP_OK;
}

int at_port_cam_qual_get(int *qmin, int *qmax)
{
    /* 契约 v1.9 §5（issue #27）：min 随当前档位收紧（板级定标） */
    if (qmin) *qmin = camera_quality_min_for(config_get_cam_framesize());
    if (qmax) *qmax = CAMERA_QUALITY_MAX;
    return config_get_cam_quality();
}

esp_err_t at_port_cam_qual_set(int value)
{
    if (value < camera_quality_min_for(config_get_cam_framesize()) ||
        value > CAMERA_QUALITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 本板生效语义（契约 §6）：热重配（同 AT+CAMRES 路径） */
    esp_err_t ret = camera_reinit(config_get_cam_framesize(), (uint8_t)value);
    if (ret != ESP_OK) {
        return ret;
    }
    at_port_write("+APPLY: hot-reconfig done\r\n");
    return ESP_OK;
}

/* ── STATUS 板级增量行 ─────────────────────────────────────────── */

void at_port_status_extra(void (*emit)(const char *name, const char *value))
{
    /* AI 开关行（PIT-022 验证口径：AI 键持久化是否真实生效） */
    char buf[48];
    snprintf(buf, sizeof(buf), "face=%s,motion=%s,qr=%s",
             ai_is_enabled(AI_FEATURE_FACE_DETECT) ? "on" : "off",
             ai_is_enabled(AI_FEATURE_MOTION_DETECT) ? "on" : "off",
             ai_is_enabled(AI_FEATURE_QR_DECODE) ? "on" : "off");
    emit("ai", buf);

    /* 双网络：当前槽位 + 备用网 SSID（旧 AT+INFO 的 Backup/Active 行） */
    emit("net", wifi_manager_active_net());
    const char *ssid2 = config_get_wifi_ssid_2();
    snprintf(buf, sizeof(buf), "%s", (ssid2 && ssid2[0]) ? ssid2 : "(none)");
    emit("wifi2", buf);
}

/* ── 系统动作 ──────────────────────────────────────────────────── */

void at_port_reboot(void)
{
    at_port_write("+REBOOTING\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void at_port_restore(void)
{
    at_port_write("+RESTORING: factory reset\r\n");
    config_reset();   /* 重置默认 + 落盘 */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── CFGGET/CFGSET 白名单（契约 §2；读侧自动剔除 secret） ──────── */

/* 读侧：类型化访问器（本仓 config_t 为 config_manager.c 私有，无
 * ai-thinker 式 struct 快照可反射，走公开 getter 单一事实源） */
static void cfg_get_device_name(char *buf, size_t len) { snprintf(buf, len, "%s", config_get_device_name()); }
static void cfg_get_wifi_ssid(char *buf, size_t len)   { snprintf(buf, len, "%s", config_get_wifi_ssid()); }
static void cfg_get_wifi_ssid_2(char *buf, size_t len) { snprintf(buf, len, "%s", config_get_wifi_ssid_2()); }
static void cfg_get_timezone(char *buf, size_t len)    { snprintf(buf, len, "%s", config_get_timezone()); }
static void cfg_get_cam_framesize(char *buf, size_t len) { snprintf(buf, len, "%u", (unsigned)config_get_cam_framesize()); }
static void cfg_get_cam_fps(char *buf, size_t len)     { snprintf(buf, len, "%u", (unsigned)config_get_cam_fps()); }
static void cfg_get_cam_quality(char *buf, size_t len) { snprintf(buf, len, "%u", (unsigned)config_get_cam_quality()); }
static void cfg_get_cam_vflip(char *buf, size_t len)   { snprintf(buf, len, "%u", config_get_cam_vflip() ? 1u : 0u); }
static void cfg_get_cam_hmirror(char *buf, size_t len) { snprintf(buf, len, "%u", config_get_cam_hmirror() ? 1u : 0u); }
static void cfg_get_cam_brightness(char *buf, size_t len) { snprintf(buf, len, "%d", (int)config_get_cam_brightness()); }
static void cfg_get_cam_contrast(char *buf, size_t len)   { snprintf(buf, len, "%d", (int)config_get_cam_contrast()); }
static void cfg_get_cam_saturation(char *buf, size_t len) { snprintf(buf, len, "%d", (int)config_get_cam_saturation()); }
static void cfg_get_cam_sharpness(char *buf, size_t len)  { snprintf(buf, len, "%d", (int)config_get_cam_sharpness()); }
static void cfg_get_xclk(char *buf, size_t len)        { snprintf(buf, len, "%u", (unsigned)config_get_xclk_freq_mhz()); }
static void cfg_get_onvif(char *buf, size_t len)       { snprintf(buf, len, "%u", config_get_onvif_enable() ? 1u : 0u); }
static void cfg_get_ap_fallback(char *buf, size_t len) { snprintf(buf, len, "%u", config_get_allow_ap_fallback() ? 1u : 0u); }
static void cfg_get_ai_face(char *buf, size_t len)     { snprintf(buf, len, "%u", config_get_ai_face_enable() ? 1u : 0u); }
static void cfg_get_ai_motion(char *buf, size_t len)   { snprintf(buf, len, "%u", config_get_ai_motion_enable() ? 1u : 0u); }
static void cfg_get_ai_qr(char *buf, size_t len)       { snprintf(buf, len, "%u", config_get_ai_qr_enable() ? 1u : 0u); }
/* CSI 调参键族（契约 v1.7；threshold 字符串 %.3f——AT_CFG 无浮点类型） */
static void cfg_get_csi_en(char *buf, size_t len)      { snprintf(buf, len, "%u", config_get_csi_enabled() ? 1u : 0u); }
static void cfg_get_csi_threshold(char *buf, size_t len) { snprintf(buf, len, "%.3f", (double)config_get_csi_threshold()); }
static void cfg_get_csi_on_hits(char *buf, size_t len) { snprintf(buf, len, "%u", (unsigned)config_get_csi_on_hits()); }
static void cfg_get_csi_off_hits(char *buf, size_t len){ snprintf(buf, len, "%u", (unsigned)config_get_csi_off_hits()); }
static void cfg_get_csi_profile(char *buf, size_t len) { snprintf(buf, len, "%u", (unsigned)config_get_csi_profile()); }
static void cfg_get_csi_heal(char *buf, size_t len)    { snprintf(buf, len, "%u", config_get_csi_auto_heal() ? 1u : 0u); }

/* 写侧通用：严格解析 + 域校验 → config_set + config_save（落盘） */

/* 字符串域校验：max_len 来自 config_key_max_len（NVS 键；allow_ap_fallback
 * 的 JSON 名与键名不同，用键名查询） */
static bool cfg_str_fits(const char *nvs_key, const char *v)
{
    size_t maxlen = config_key_max_len(nvs_key);
    return maxlen > 0 && strlen(v) <= maxlen - 1;
}

static esp_err_t cfg_set_str_field(const char *nvs_key, const char *v)
{
    if (!cfg_str_fits(nvs_key, v)) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_write(nvs_key, v);
}

static esp_err_t cfg_set_device_name(const char *v)
{
    if (!v[0]) return ESP_ERR_INVALID_ARG;
    return cfg_set_str_field("device_name", v);
}
static esp_err_t cfg_set_wifi_ssid(const char *v)
{
    return cfg_set_str_field("wifi_ssid", v);
}
static esp_err_t cfg_set_wifi_pass(const char *v)
{
    return cfg_set_str_field("wifi_pass", v);
}
static esp_err_t cfg_set_wifi_ssid_2(const char *v)
{
    return cfg_set_str_field("wifi_ssid_2", v);
}
static esp_err_t cfg_set_wifi_pass_2(const char *v)
{
    return cfg_set_str_field("wifi_pass_2", v);
}
static esp_err_t cfg_set_timezone(const char *v)
{
    size_t maxlen = config_key_max_len("timezone");
    if (!v[0] || strlen(v) > maxlen - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = cfg_write("timezone", v);
    if (ret == ESP_OK) {
        /* 立即生效（同 POST /api/config；本板无 NTP，供手动设时换算） */
        setenv("TZ", v, 1);
        tzset();
    }
    return ret;
}
static esp_err_t cfg_set_cam_framesize(const char *v)
{
    long val;
    if (!parse_long_strict(v, &val)) {
        return ESP_ERR_INVALID_ARG;
    }
    return at_port_cam_res_set((int)val) == ESP_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}
static esp_err_t cfg_set_cam_fps(const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || val < 1 || val > 30) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    return cfg_write("cam_fps", buf);
}
static esp_err_t cfg_set_cam_quality(const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) ||
        val < camera_quality_min_for(config_get_cam_framesize()) || val > CAMERA_QUALITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 本板语义：画质变更热重配（同 AT+CAMQUAL） */
    return at_port_cam_qual_set((int)val) == ESP_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}
/* 传感器微调 + 翻转：写后 live 应用（同 POST /api/camera 语义） */
static esp_err_t cfg_set_sensor_i8(const char *key, const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || val < -2 || val > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    esp_err_t ret = cfg_write(key, buf);
    if (ret == ESP_OK) {
        camera_apply_sensor_settings();
    }
    return ret;
}
static esp_err_t cfg_set_cam_brightness(const char *v) { return cfg_set_sensor_i8("cam_brightness", v); }
static esp_err_t cfg_set_cam_contrast(const char *v)   { return cfg_set_sensor_i8("cam_contrast", v); }
static esp_err_t cfg_set_cam_saturation(const char *v) { return cfg_set_sensor_i8("cam_saturation", v); }
static esp_err_t cfg_set_cam_sharpness(const char *v)  { return cfg_set_sensor_i8("cam_sharpness", v); }
static esp_err_t cfg_set_sensor_bool(const char *key, const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || (val != 0 && val != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    esp_err_t ret = cfg_write(key, buf);
    if (ret == ESP_OK) {
        camera_apply_sensor_settings();
    }
    return ret;
}
static esp_err_t cfg_set_cam_vflip(const char *v)   { return cfg_set_sensor_bool("cam_vflip", v); }
static esp_err_t cfg_set_cam_hmirror(const char *v) { return cfg_set_sensor_bool("cam_hmirror", v); }
static esp_err_t cfg_set_u8_bool_field(const char *key, const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || (val != 0 && val != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    return cfg_write(key, buf);
}
static esp_err_t cfg_set_xclk(const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || (val != 10 && val != 16 && val != 20)) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    /* 下次 camera init/reinit 生效（同 POST /api/config 语义） */
    return cfg_write("xclk_freq_mhz", buf);
}
/* CSI 写路径：u8 键 cfg_write（百分刻度换算在 threshold setter）→ 热应用 */
static esp_err_t csi_write_and_apply(const char *key, const char *val)
{
    esp_err_t ret = cfg_write(key, val);
    if (ret == ESP_OK) csi_motion_apply_config();
    return ret;
}
static esp_err_t cfg_set_csi_en(const char *v)
{
    return (v[0]=='0'||v[0]=='1') && v[1]=='\0' ? csi_write_and_apply("csi_enabled", v) : ESP_ERR_INVALID_ARG;
}
static esp_err_t cfg_set_csi_threshold(const char *v)
{
    char *end = NULL;
    float t = strtof(v, &end);
    if (end == v || *end != '\0') return ESP_ERR_INVALID_ARG;
    if (t != 0.0f && (t < 0.05f || t > 1.0f)) return ESP_ERR_INVALID_ARG;
    char pct[8];
    snprintf(pct, sizeof(pct), "%d", (int)(t * 100.0f + 0.5f));
    const float prev = config_get_csi_threshold();
    esp_err_t ret = csi_write_and_apply("csi_threshold", pct);
    if (ret == ESP_OK && t == 0.0f && prev > 0.0f) csi_motion_set_threshold(0.0f);
    return ret;
}
static esp_err_t cfg_set_csi_hits(const char *key, const char *v)
{
    int n = atoi(v);
    if (n < 1 || n > 20) return ESP_ERR_INVALID_ARG;
    return csi_write_and_apply(key, v);
}
static esp_err_t cfg_set_csi_on_hits(const char *v)  { return cfg_set_csi_hits("csi_on_hits", v); }
static esp_err_t cfg_set_csi_off_hits(const char *v) { return cfg_set_csi_hits("csi_off_hits", v); }
static esp_err_t cfg_set_csi_profile(const char *v)
{
    return (v[0]=='0'||v[0]=='1') && v[1]=='\0' ? csi_write_and_apply("csi_profile", v) : ESP_ERR_INVALID_ARG;
}
static esp_err_t cfg_set_csi_heal(const char *v)
{
    return (v[0]=='0'||v[0]=='1') && v[1]=='\0' ? csi_write_and_apply("csi_auto_heal", v) : ESP_ERR_INVALID_ARG;
}
static esp_err_t cfg_set_onvif(const char *v)
{
    return cfg_set_u8_bool_field("onvif_enable", v);   /* 重启生效（同 web） */
}
static esp_err_t cfg_set_ap_fallback(const char *v)
{
    return cfg_set_u8_bool_field("ap_fallback", v);
}
/* AI 开关：写后 live 应用（同 POST /api/ai 语义：持久化 + ai_enable） */
static esp_err_t cfg_set_ai_field(const char *key, ai_feature_t feat, const char *v)
{
    long val;
    if (!parse_long_strict(v, &val) || (val != 0 && val != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", val);
    esp_err_t ret = cfg_write(key, buf);
    if (ret == ESP_OK) {
        ai_enable(feat, val == 1);
    }
    return ret;
}
static esp_err_t cfg_set_ai_face(const char *v)
{
    return cfg_set_ai_field("ai_face_en", AI_FEATURE_FACE_DETECT, v);
}
static esp_err_t cfg_set_ai_motion(const char *v)
{
    return cfg_set_ai_field("ai_motion_en", AI_FEATURE_MOTION_DETECT, v);
}
static esp_err_t cfg_set_ai_qr(const char *v)
{
    return cfg_set_ai_field("ai_qr_en", AI_FEATURE_QR_DECODE, v);
}
/* 白名单表（get 仅非 secret 字段；set 含 secret 写入；名字 = 契约 JSON 名） */
static const at_cfg_field_t s_cfg_fields[] = {
    { "device_name",      AT_CFG_STR, false, cfg_get_device_name,  cfg_set_device_name },
    { "wifi_ssid",        AT_CFG_STR, false, cfg_get_wifi_ssid,    cfg_set_wifi_ssid },
    { "wifi_pass",        AT_CFG_STR, true,  NULL,                 cfg_set_wifi_pass },
    { "wifi_ssid_2",      AT_CFG_STR, false, cfg_get_wifi_ssid_2,  cfg_set_wifi_ssid_2 },
    { "wifi_pass_2",      AT_CFG_STR, true,  NULL,                 cfg_set_wifi_pass_2 },
    { "timezone",         AT_CFG_STR, false, cfg_get_timezone,     cfg_set_timezone },
    { "cam_framesize",    AT_CFG_U8,  false, cfg_get_cam_framesize, cfg_set_cam_framesize },
    { "cam_fps",          AT_CFG_U8,  false, cfg_get_cam_fps,      cfg_set_cam_fps },
    { "cam_quality",      AT_CFG_U8,  false, cfg_get_cam_quality,  cfg_set_cam_quality },
    { "cam_vflip",        AT_CFG_U8,  false, cfg_get_cam_vflip,    cfg_set_cam_vflip },
    { "cam_hmirror",      AT_CFG_U8,  false, cfg_get_cam_hmirror,  cfg_set_cam_hmirror },
    { "cam_brightness",   AT_CFG_I8,  false, cfg_get_cam_brightness, cfg_set_cam_brightness },
    { "cam_contrast",     AT_CFG_I8,  false, cfg_get_cam_contrast,   cfg_set_cam_contrast },
    { "cam_saturation",   AT_CFG_I8,  false, cfg_get_cam_saturation, cfg_set_cam_saturation },
    { "cam_sharpness",    AT_CFG_I8,  false, cfg_get_cam_sharpness,  cfg_set_cam_sharpness },
    { "xclk_freq_mhz",    AT_CFG_U8,  false, cfg_get_xclk,          cfg_set_xclk },
    { "onvif_enable",     AT_CFG_U8,  false, cfg_get_onvif,         cfg_set_onvif },
    /* CSI 调参键族（契约 v1.7） */
    { "csi_enabled",     AT_CFG_U8,  false, cfg_get_csi_en,        cfg_set_csi_en },
    { "csi_threshold",   AT_CFG_STR, false, cfg_get_csi_threshold, cfg_set_csi_threshold },
    { "csi_on_hits",     AT_CFG_U8,  false, cfg_get_csi_on_hits,   cfg_set_csi_on_hits },
    { "csi_off_hits",    AT_CFG_U8,  false, cfg_get_csi_off_hits,  cfg_set_csi_off_hits },
    { "csi_profile",     AT_CFG_U8,  false, cfg_get_csi_profile,   cfg_set_csi_profile },
    { "csi_auto_heal",   AT_CFG_U8,  false, cfg_get_csi_heal,      cfg_set_csi_heal },
    { "allow_ap_fallback", AT_CFG_U8, false, cfg_get_ap_fallback,   cfg_set_ap_fallback },
    { "ai_face_en",       AT_CFG_U8,  false, cfg_get_ai_face,       cfg_set_ai_face },
    { "ai_motion_en",     AT_CFG_U8,  false, cfg_get_ai_motion,     cfg_set_ai_motion },
    { "ai_qr_en",         AT_CFG_U8,  false, cfg_get_ai_qr,         cfg_set_ai_qr },
};

const at_cfg_field_t *at_port_cfg_fields(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_cfg_fields) / sizeof(s_cfg_fields[0]));
    }
    return s_cfg_fields;
}

void at_port_save(void)
{
    /* 本仓写路径 config_set+config_save 已落盘；AT+SAVE 再落一次幂等保平安
     * （契约 §2：写路径自带落盘时为幂等操作） */
    config_save();
}

/* ── 历史别名（契约 §4；本仓历史名：CONFIG/INFO → STATUS，CIFSR → IP） ── */

const char *at_port_alias(const char *name)
{
    if (strcasecmp(name, "CONFIG") == 0) return "STATUS";
    if (strcasecmp(name, "INFO") == 0)   return "STATUS";
    if (strcasecmp(name, "CIFSR") == 0)  return "IP";
    return NULL;
}

/* ── 板级扩展指令（契约 §5 n16r8 登记项） ──────────────────────── */

static void ext_ok(void)   { at_port_write("OK\r\n"); }
static void ext_err(const char *why)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "ERROR: %s\r\n", why ? why : "");
    at_port_write(buf);
}

/* AI 开关三连（AIFACE / AIMOTION / AIQR）：? 查询 live 态，=on/off 写入
 * （持久化 + live 应用，同 POST /api/ai）。
 * 入参是含指令名的完整命令串（如 "AIFACE=on"），先跳过自身前缀。 */
static esp_err_t ext_ai(ai_feature_t feat, const char *cmd, const char *name)
{
    cmd += strlen(name);
    if (cmd[0] == '\0' || cmd[0] == '?') {
        char line[40];
        snprintf(line, sizeof(line), "+%s: %s\r\n", name,
                 ai_is_enabled(feat) ? "on" : "off");
        at_port_write(line);
        ext_ok();
        return ESP_OK;
    }
    if (cmd[0] == '=') {
        bool on;
        if (!parse_onoff(cmd + 1, &on)) {
            ext_err("use on/off");
            return ESP_OK;
        }
        const char *key = (feat == AI_FEATURE_FACE_DETECT)  ? "ai_face_en" :
                          (feat == AI_FEATURE_MOTION_DETECT) ? "ai_motion_en" : "ai_qr_en";
        esp_err_t ret = cfg_write(key, on ? "1" : "0");
        if (ret != ESP_OK) {
            ext_err("save failed");
            return ESP_OK;
        }
        ai_enable(feat, on);
        ext_ok();
        return ESP_OK;
    }
    ext_err("usage: AT+NAME? | AT+NAME=on/off");
    return ESP_OK;
}

static esp_err_t ext_aiface(const char *cmd)   { return ext_ai(AI_FEATURE_FACE_DETECT, cmd, "AIFACE"); }
static esp_err_t ext_aimotion(const char *cmd) { return ext_ai(AI_FEATURE_MOTION_DETECT, cmd, "AIMOTION"); }
static esp_err_t ext_aiqr(const char *cmd)     { return ext_ai(AI_FEATURE_QR_DECODE, cmd, "AIQR"); }

/* WIFI2：备用网络凭据。查询脱敏（红线 §3）；"=,"（空 ssid）清除备用网；
 * 任何写入保存后 500ms 重启（凭据启动时读取，承袭旧语义） */
static esp_err_t ext_wifi2(const char *cmd)
{
    cmd += strlen("WIFI2");
    if (cmd[0] == '\0' || cmd[0] == '?') {
        const char *ssid = config_get_wifi_ssid_2();
        char line[64];
        snprintf(line, sizeof(line), "+WIFI2: ssid:%s\r\n",
                 (ssid && ssid[0]) ? ssid : "(not set)");
        at_port_write(line);
        snprintf(line, sizeof(line), "+WIFI2: pass:%s\r\n",
                 (config_get_wifi_pass_2() && config_get_wifi_pass_2()[0]) ? "****" : "(empty)");
        at_port_write(line);
        snprintf(line, sizeof(line), "+WIFI2: net:%s\r\n", wifi_manager_active_net());
        at_port_write(line);
        ext_ok();
        return ESP_OK;
    }
    if (cmd[0] != '=') {
        ext_err("usage: AT+WIFI2? | AT+WIFI2=ssid,pass (empty ssid clears)");
        return ESP_OK;
    }
    /* 本地副本上切分（不改核心传入的行缓冲）；32(ssid)+1+64(pass) 上限 */
    char args[128];
    strlcpy(args, cmd + 1, sizeof(args));
    char *ssid = args;
    char *comma = strchr(ssid, ',');
    if (!comma) {
        ext_err("usage: AT+WIFI2=ssid,pass (empty pass: ssid,)");
        return ESP_OK;
    }
    *comma = '\0';
    char *pass = comma + 1;
    esp_err_t ret;
    if (strlen(ssid) == 0) {
        /* 空 ssid：清除备用网络 */
        ret = cfg_write("wifi_ssid_2", "");
        if (ret == ESP_OK) {
            ret = cfg_write("wifi_pass_2", "");
        }
    } else {
        ret = cfg_write("wifi_ssid_2", ssid);
        if (ret == ESP_OK) {
            ret = cfg_write("wifi_pass_2", pass);
        }
    }
    if (ret != ESP_OK) {
        ext_err("save failed");
        return ESP_OK;
    }
    ext_ok();
    at_port_write("+REBOOTING: backup network saved\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* LED：查询亮度 / 设置 0-100（简单 GPIO 语义 0=灭 >0=亮，见 flash_led.c） */
static esp_err_t ext_led(const char *cmd)
{
    cmd += strlen("LED");
    if (cmd[0] == '\0' || cmd[0] == '?') {
        char line[32];
        snprintf(line, sizeof(line), "+LED: %u%%\r\n", (unsigned)flash_led_get_brightness());
        at_port_write(line);
        ext_ok();
        return ESP_OK;
    }
    if (cmd[0] == '=') {
        long val;
        if (!parse_long_strict(cmd + 1, &val) || val < 0 || val > 100) {
            ext_err("brightness must be 0-100");
            return ESP_OK;
        }
        esp_err_t ret = flash_led_set_brightness((uint8_t)val);
        if (ret != ESP_OK) {
            ext_err(esp_err_to_name(ret));
            return ESP_OK;
        }
        ext_ok();
        return ESP_OK;
    }
    ext_err("usage: AT+LED? | AT+LED=n (0-100)");
    return ESP_OK;
}

/* CAMCAP：拍一帧报告尺寸（承袭旧 AT+CAMCAP） */
static esp_err_t ext_camcap(const char *cmd)
{
    cmd += strlen("CAMCAP");
    if (cmd[0] != '\0') {
        ext_err("usage: AT+CAMCAP");
        return ESP_OK;
    }
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        ext_err("capture failed");
        return ESP_OK;
    }
    char line[48];
    snprintf(line, sizeof(line), "+CAMCAP: %ux%u len=%u\r\n",
             fb->width, fb->height, (unsigned)fb->len);
    esp_camera_fb_return(fb);
    at_port_write(line);
    ext_ok();
    return ESP_OK;
}

/* AT+CHHEALTH?（信道健康快照）/ =SCAN（手动拥塞 scan）——契约 v1.3 ①b */
static esp_err_t ext_chhealth(const char *cmd)
{
    char line[144];
    if (strncasecmp(cmd, "CHHEALTH?", 10) == 0) {
        wifi_chan_health_t ch;
        if (!wifi_channel_health_get(&ch)) {
            at_port_write("+ERROR: chan health not sampled yet\r\n");
            return ESP_OK;
        }
        snprintf(line, sizeof(line),
                 "+CHHEALTH: rssi %d/%d dBm ch=%u disc_1h=%u busy=%u%% bss=%u/%u\r\n",
                 (int)ch.rssi_avg, (int)ch.rssi_min, (unsigned)ch.channel,
                 (unsigned)ch.disconnects_1h, (unsigned)ch.busy_score,
                 (unsigned)ch.bss_on_chan, (unsigned)ch.bss_total);
        at_port_write(line);
        snprintf(line, sizeof(line), "+CHHEALTH: csi_adm=%.1f pps cb_ratio=%.1f scan_ts=%u\r\n",
                 (double)ch.csi_adm_pps, (double)ch.csi_cb_ratio, (unsigned)ch.scan_ts);
        at_port_write(line);
        at_port_write("OK\r\n");
        return ESP_OK;
    }
    if (strncasecmp(cmd, "CHHEALTH=SCAN", 13) == 0) {
        esp_err_t ret = wifi_channel_health_scan_now();
        if (ret != ESP_OK) {
            at_port_write("+ERROR: busy (stream clients watching)\r\n");
        } else {
            at_port_write("+CHHEALTH: scan queued (see CHHEALTH? in ~15s)\r\nOK\r\n");
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* AT+CSI?（实时快照）/ AT+CSICAL（重校准）——契约 v1.3 */
static esp_err_t ext_csi(const char *cmd)
{
    if (strncasecmp(cmd, "CSI?", 4) != 0) return ESP_ERR_NOT_SUPPORTED;
    csi_motion_status_t st;
    char line[128];
    if (!csi_motion_get_status(&st)) {
        at_port_write("+ERROR: CSI sensing not active (warming or not built)\r\n");
        return ESP_OK;
    }
    snprintf(line, sizeof(line),
             "+CSI: state=%s score=%.3f thr=%.3f profile=%u locked=%d calibrating=%d\r\n",
             st.state, (double)st.score, (double)st.thr,
             (unsigned)st.profile, st.thr_locked, st.calibrating);
    at_port_write(line);
    snprintf(line, sizeof(line),
             "+CSI: flip_rate=%u/h tx=%.1f cb=%.1f adm=%.1f pps\r\n",
             (unsigned)st.flip_rate, (double)st.tx_pps, (double)st.cb_pps,
             (double)st.adm_pps);
    at_port_write(line);
    at_port_write("OK\r\n");
    return ESP_OK;
}
static esp_err_t ext_csical(const char *cmd)
{
    if (strncasecmp(cmd, "CSICAL", 6) != 0) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t ret = csi_motion_recalibrate();
    if (ret != ESP_OK) {
        at_port_write(ret == ESP_ERR_NOT_SUPPORTED
                          ? "+ERROR: CSI sensing not built\r\n"
                          : "+ERROR: CSI runtime not ready\r\n");
    } else {
        at_port_write("+CSICAL: recalibration started\r\nOK\r\n");
    }
    return ESP_OK;
}

static const at_ext_cmd_t s_ext_cmds[] = {
    { "CSI",    "CSI? (live sensing snapshot)",     ext_csi    },
    { "CSICAL", "CSICAL (trigger recalibration)",   ext_csical },
    { "CHHEALTH", "CHHEALTH? | CHHEALTH=SCAN (channel health)", ext_chhealth },
    { "AIFACE",   "AIFACE? | AIFACE=on/off",          ext_aiface   },
    { "AIMOTION", "AIMOTION? | AIMOTION=on/off",      ext_aimotion },
    { "AIQR",     "AIQR? | AIQR=on/off",              ext_aiqr     },
    { "WIFI2",    "WIFI2? | WIFI2=ssid,pass (empty ssid clears)", ext_wifi2 },
    { "LED",      "LED? | LED=n (0-100 percent)",     ext_led      },
    { "CAMCAP",   "capture one frame, report size",   ext_camcap   },
};

const at_ext_cmd_t *at_port_ext_cmds(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_ext_cmds) / sizeof(s_ext_cmds[0]));
    }
    return s_ext_cmds;
}
