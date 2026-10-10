/*
 * MiBee usb-webcam — ESP32-S3-N16R8 板 UVC 摄像头固件
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 结构改编自 espressif/esp-iot-solution examples/usb/device/usb_webcam
 * （Apache-2.0）：esp_camera 采集 JPEG → usb_device_uvc 回调直推 UVC。
 * 本板差异：GOOUUU 引脚表（camera_pin.h）、OV5640 实载、XCLK 16MHz
 * （cam 仓上板结论）、主任务订阅 TWDT（固件基线规范）。
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_task_wdt.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "camera_pin.h"
#include "esp_camera.h"
#include "usb_device_uvc.h"
#include "uvc_frame_config.h"

static const char *TAG = "usb_webcam";

#define CAMERA_XCLK_FREQ  CONFIG_CAMERA_XCLK_FREQ
#define CAMERA_FB_COUNT   2

/* USB Full-Speed 帧预算：VGA MJPEG q12 典型 20-40KB，75KB 上限有余量
 * （esp-iot-solution 对 S3 的同款取值） */
#define UVC_MAX_FRAMESIZE_SIZE  (75 * 1024)

/**
 * @brief  SCCB 总线扫描（诊断用）：枚举 SIOD/SIOC 上所有应答的 7bit 地址。
 *         换摄像头模块探测失败时区分三种情况：无任何应答=没插到位/插反/
 *         供电不通；有应答但驱动不认=型号不支持（看地址猜传感器家族）；
 *         应答且被驱动认出=正常。独立于 esp32-camera 的 SCCB 实现，
 *         扫完即拆总线，不与后续 camera init 冲突。
 *         部分传感器没有 XCLK 就不应答 I2C（内部逻辑由 XCLK 供时钟）——
 *         扫描前先用 LEDC 起 XCLK、连扫 ROUNDS 遍抓边缘性接触。
 */
#define SCCB_SCAN_ROUNDS 7
static void sccb_scan(void)
{
    /* XCLK 先行（与 esp_camera 同 timer/channel，后续 init 会重配，不冲突）。
     * 六轮二分定位 sccb-ng 与裸 i2c_master 的行为差异：
     *   A 动态端口（对照）        B 显式端口0
     *   C 显式端口1              D 完整复刻 sccb-ng 配置
     *   E = D + 先 i2c_master_probe（sccb-ng 的实际时序）
     *   F = E 但用纯写（i2c_master_transmit，sccb-ng 的第一个操作） */
    struct { int port; bool pullup; bool probe_first; bool pure_write; const char *tag; } variants[SCCB_SCAN_ROUNDS] = {
        {-1, false, false, false, "A dyn-port "},
        { 0, false, false, false, "B port0    "},
        { 1, false, false, false, "C port1    "},
        { 1, true,  false, false, "D sccb-cfg "},
        { 1, true,  true,  false, "E probe+rw  "},
        { 1, true,  true,  true,  "F probe+wr  "},
        { 1, true,  true,  true,  "G get-bus   "},
    };
    uint32_t mhz = 16;
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = mhz * 1000000,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t chcfg = {
        .gpio_num   = CAMERA_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 1,
        .hpoint     = 0,
    };
    ledc_channel_config(&chcfg);

    for (int round = 0; round < SCCB_SCAN_ROUNDS; round++) {
        i2c_master_bus_handle_t bus = NULL;
        esp_err_t err;
        if (round == 5) {
            /* G：复刻 sccb-ng 的总线找回方式——i2c_master_get_bus_handle
             * 按端口号取回（前面 G 位序：round5 创建在先、句柄重新取回） */
            i2c_master_bus_config_t cfg5 = {
                .i2c_port = 1,
                .sda_io_num = CAMERA_PIN_SIOD,
                .scl_io_num = CAMERA_PIN_SIOC,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags = { .enable_internal_pullup = true },
            };
            err = i2c_new_master_bus(&cfg5, &bus);
            if (err == ESP_OK) {
                i2c_master_bus_handle_t fetched = NULL;
                esp_err_t ge = i2c_master_get_bus_handle(1, &fetched);
                ESP_LOGW(TAG, "[G get-bus ] new:%s fetch:%s (same=%d)",
                         esp_err_to_name(err), esp_err_to_name(ge), fetched == bus);
                bus = fetched;   /* 用取回的句柄继续（sccb-ng 同款） */
            }
        } else {
            i2c_master_bus_config_t bus_cfg = {
                .i2c_port = variants[round].port,
                .sda_io_num = CAMERA_PIN_SIOD,
                .scl_io_num = CAMERA_PIN_SIOC,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags = { .enable_internal_pullup = variants[round].pullup },
            };
            err = i2c_new_master_bus(&bus_cfg, &bus);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[%s] bus init %s", variants[round].tag, esp_err_to_name(err));
            continue;
        }
        if (variants[round].probe_first) {
            esp_err_t pe = i2c_master_probe(bus, 0x30, pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[%s] i2c_master_probe(0x30): %s", variants[round].tag,
                     esp_err_to_name(pe));
        }
        uint8_t pid = 0;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x30,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            esp_err_t e;
            if (variants[round].pure_write) {
                uint8_t bank = 0x01;
                esp_err_t e1 = i2c_master_transmit(dev, (uint8_t[]){0xFF, bank}, 2,
                                                  pdMS_TO_TICKS(1000));
                e = i2c_master_transmit_receive(dev, (uint8_t[]){0x0A}, 1, &pid, 1,
                                                pdMS_TO_TICKS(1000));
                ESP_LOGI(TAG, "[%s] wr(0xFF=01):%s rd(0x0A):%s (0x%02X)",
                         variants[round].tag, esp_err_to_name(e1),
                         esp_err_to_name(e), pid);
            } else {
                e = i2c_master_transmit_receive(dev, (uint8_t[]){0x0A}, 1, &pid, 1,
                                                pdMS_TO_TICKS(50));
                ESP_LOGI(TAG, "[%s] PID@0x30: %s (0x%02X)", variants[round].tag,
                         esp_err_to_name(e), pid);
            }
            i2c_master_bus_rm_device(dev);
        } else {
            ESP_LOGW(TAG, "[%s] add device failed", variants[round].tag);
        }
        i2c_del_master_bus(bus);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

typedef struct {
    camera_fb_t *cam_fb_p;
    uvc_fb_t uvc_fb;
} fb_t;

static fb_t s_fb;

static esp_err_t camera_init(uint32_t xclk_freq_hz, pixformat_t pixel_format,
                             framesize_t frame_size, int jpeg_quality, uint8_t fb_count)
{
    /* 去重 + 换参重配（上游例程同款）：esp_camera_init 连调两次是未定义
     * 行为——分辨率切换必须先 return_all + deinit 再重 init。 */
    static bool inited = false;
    static uint32_t cur_xclk = 0;
    static framesize_t cur_size = 0;
    static uint8_t cur_fb_count = 0;

    if (inited && cur_xclk == xclk_freq_hz && cur_size == frame_size
        && cur_fb_count == fb_count) {
        ESP_LOGD(TAG, "camera already inited");
        return ESP_OK;
    }
    if (inited) {
        esp_camera_return_all();
        esp_camera_deinit();
        inited = false;
        ESP_LOGI(TAG, "camera RESTART (reconfig)");
    }

    camera_config_t camera_config = {
        .pin_pwdn     = CAMERA_PIN_PWDN,
        .pin_reset    = CAMERA_PIN_RESET,
        .pin_sscb_sda = CAMERA_PIN_SIOD,
        .pin_sscb_scl = CAMERA_PIN_SIOC,

        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href  = CAMERA_PIN_HREF,
        .pin_pclk  = CAMERA_PIN_PCLK,

        .xclk_freq_hz = xclk_freq_hz,
        /* pin_xclk=-1：外部供时钟模式——ll_cam_config 不接管 XCLK 引脚
         * （S3 上它接的 LCD_CAM CAM_CLK 在 cam_init 期是 cam_clk_sel=3
         * "no clock"，会把探测期的引脚变成死时钟——杂牌 OV2640 没活
         * XCLK 就不理 SCCB，2026-10-10 定位的探测恒败根因）。XCLK 由本
         * 函数开头的 LEDC 永久供给。 */
        .pin_xclk     = -1,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = pixel_format,
        .frame_size   = frame_size,

        .jpeg_quality = jpeg_quality,
        .fb_count     = fb_count,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };

    esp_err_t ret = ESP_FAIL;
    /* S3 探测期 XCLK 缺失修复（2026-10-10 换 OV2640 模组定位）：
     * esp32-camera 在 S3 上 CAMERA_ENABLE_OUT_CLOCK 是空宏（假设
     * LCD_CAM 出 XCLK），而 LCD_CAM 要到 cam_config——探测成功之后—
     * 才配置。探测阶段 XCLK 死寂：OV5640 无 XCLK 也应答 SCCB（cam
     * 固件因此能过），本 OV2640 模组必须有活 XCLK 才理寄存器 → 探测
     * 恒败、自备 LEDC XCLK 的裸读恒通。修法：探测前先开 LEDC XCLK，
     * 成功后停掉把引脚交棒给 LCD_CAM（cam_config 会 gpio_matrix 接管）。
     * 保留 3×200ms 重试兜底。 */
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = xclk_freq_hz * 1000000,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t chcfg = {
        .gpio_num   = CAMERA_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 1,
        .hpoint     = 0,
    };
    ledc_channel_config(&chcfg);
    vTaskDelay(pdMS_TO_TICKS(20));   /* 传感器见活时钟后再被探测 */

    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = esp_camera_init(&camera_config);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_camera_init attempt %d/3 failed: %s",
                 attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ret != ESP_OK) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        return ret;
    }
    /* LEDC XCLK 不停——本固件全程由它供传感器主时钟（pin_xclk=-1 外部
     * 供时钟模式，LCD_CAM 不会接管引脚） */

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "sensor detected: PID=0x%x (%s)", s->id.PID,
             (s->id.PID == OV5640_PID) ? "OV5640" :
             (s->id.PID == OV3660_PID) ? "OV3660" :
             (s->id.PID == OV2640_PID) ? "OV2640" : "other");
#if CONFIG_CAMERA_VFLIP
    s->set_vflip(s, 1);
#endif
#if CONFIG_CAMERA_HMIRROR
    s->set_hmirror(s, 1);
#endif

    camera_sensor_info_t *s_info = esp_camera_sensor_get_info(&(s->id));
    if (s_info == NULL || !s_info->support_jpeg) {
        ESP_LOGE(TAG, "sensor does not support JPEG output");
        esp_camera_deinit();   /* 探测成功但不合用：干净退出，别留半初始化状态 */
        return ESP_ERR_NOT_SUPPORTED;
    }
    cur_xclk = xclk_freq_hz;
    cur_size = frame_size;
    cur_fb_count = fb_count;
    inited = true;
    return ESP_OK;
}

/* 宿主（PC）打开摄像头时按协商参数初始化传感器——分辨率切换走
 * deinit/init 全量重配（cam 仓"协调式重配"教训：热改参数会踩无效状态）。 */
static esp_err_t camera_start_cb(uvc_format_t format, int width, int height,
                                 int rate, void *cb_ctx)
{
    (void)cb_ctx;
    ESP_LOGI(TAG, "Camera Start: format=%d %dx%d @%dfps", format, width, height, rate);

    if (format != UVC_FORMAT_JPEG) {
        ESP_LOGE(TAG, "only MJPEG format is supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    framesize_t frame_size;
    int jpeg_quality;
    if (width == 320 && height == 240) {
        frame_size = FRAMESIZE_QVGA;
        jpeg_quality = 10;
    } else if (width == 480 && height == 320) {
        frame_size = FRAMESIZE_HVGA;
        jpeg_quality = 10;
    } else if (width == 640 && height == 480) {
        frame_size = FRAMESIZE_VGA;
        jpeg_quality = 12;
    } else if (width == 800 && height == 600) {
        frame_size = FRAMESIZE_SVGA;
        jpeg_quality = 14;
    } else {
        ESP_LOGE(TAG, "unsupported frame size %dx%d", width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return camera_init(CAMERA_XCLK_FREQ, PIXFORMAT_JPEG, frame_size,
                       jpeg_quality, CAMERA_FB_COUNT);
}

static void camera_stop_cb(void *cb_ctx)
{
    (void)cb_ctx;
    ESP_LOGI(TAG, "Camera Stop");
    /* 传感器保持初始化（下次 open 重配即可），不_deinit：省 1-2s 重启开销 */
}

static uvc_fb_t *camera_fb_get_cb(void *cb_ctx)
{
    (void)cb_ctx;
    s_fb.cam_fb_p = esp_camera_fb_get();
    if (!s_fb.cam_fb_p) {
        return NULL;
    }
    s_fb.uvc_fb.buf       = s_fb.cam_fb_p->buf;
    s_fb.uvc_fb.len       = s_fb.cam_fb_p->len;
    s_fb.uvc_fb.width     = s_fb.cam_fb_p->width;
    s_fb.uvc_fb.height    = s_fb.cam_fb_p->height;
    s_fb.uvc_fb.format    = s_fb.cam_fb_p->format;
    s_fb.uvc_fb.timestamp = s_fb.cam_fb_p->timestamp;

    if (s_fb.uvc_fb.len > UVC_MAX_FRAMESIZE_SIZE) {
        ESP_LOGE(TAG, "frame %u B over UVC budget %u B", s_fb.uvc_fb.len,
                 (unsigned)UVC_MAX_FRAMESIZE_SIZE);
        esp_camera_fb_return(s_fb.cam_fb_p);
        return NULL;
    }
    return &s_fb.uvc_fb;
}

static void camera_fb_return_cb(uvc_fb_t *fb, void *cb_ctx)
{
    (void)cb_ctx;
    assert(fb == &s_fb.uvc_fb);
    esp_camera_fb_return(s_fb.cam_fb_p);
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiBee usb-webcam on %s (XCLK=%dMHz, UVC %dx%d @%dfps bulk)",
             CAMERA_MODULE_NAME, CAMERA_XCLK_FREQ,
             UVC_FRAMES_INFO[0][0].width, UVC_FRAMES_INFO[0][0].height,
             UVC_FRAMES_INFO[0][0].rate);

    /* 开机传感器自检：驱动探测先行（干净条件——预扫描的动态 I2C 总线
     * 会搅驱动用的 SCCB 端口 1，2026-10-10 实测探测从 15/15 全通变全超时）；
     * 探测失败才跑 SCCB 扫描做物理层诊断。失败只告警不阻断——UVC 照常
     * 枚举，宿主打开时 camera_start_cb 会重试（失败路径 esp_camera_deinit
     * 已清理，可安全重入）。 */
    esp_err_t probe = camera_init(CAMERA_XCLK_FREQ, PIXFORMAT_JPEG,
                                  FRAMESIZE_VGA, 12, CAMERA_FB_COUNT);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "boot camera probe failed: %s", esp_err_to_name(probe));
        sccb_scan();
    }

    /* SELFTEST 自检行（工作区 AGENTS.md"固件自检行"规范）：一行可 grep
     * 的开机体检证据，serialtap 按前缀聚合做台架异常发现。本固件无
     * WiFi，自检行只有板/传感器部分。 */
    {
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        uint32_t flsz = 0;
        esp_flash_get_size(NULL, &flsz);   /* v6：NULL=默认主 flash */
        size_t psz = esp_psram_get_size();
        sensor_t *s = esp_camera_sensor_get();
        char sensor[32] = "none";
        if (s && s->id.PID) {
            snprintf(sensor, sizeof(sensor), "OV%04x/0x%04X",
                     s->id.PID, s->id.PID);
        }
        char psram_str[12];
        if (psz) snprintf(psram_str, sizeof(psram_str), "%uMB", (unsigned)(psz >> 20));
        else     strlcpy(psram_str, "none", sizeof(psram_str));
        ESP_LOGI(TAG, "SELFTEST: board=esp32s3-n16r8-usb-webcam fw=v0.1"
                      " chip=%s rev=v%d.%d cores=%u flash=%uMB psram=%s"
                      " sensor=%s heap=%uKB",
                 CONFIG_IDF_TARGET, ci.revision / 100, ci.revision % 100,
                 ci.cores, (unsigned)(flsz >> 20), psram_str,
                 sensor, (unsigned)(esp_get_free_heap_size() >> 10));
    }

    uint8_t *uvc_buffer = (uint8_t *)malloc(UVC_MAX_FRAMESIZE_SIZE);
    if (uvc_buffer == NULL) {
        ESP_LOGE(TAG, "uvc buffer malloc failed");
        return;
    }

    uvc_device_config_t config = {
        .uvc_buffer      = uvc_buffer,
        .uvc_buffer_size = UVC_MAX_FRAMESIZE_SIZE,
        .start_cb        = camera_start_cb,
        .fb_get_cb       = camera_fb_get_cb,
        .fb_return_cb    = camera_fb_return_cb,
        .stop_cb         = camera_stop_cb,
    };

    ESP_ERROR_CHECK(uvc_device_config(0, &config));
    ESP_ERROR_CHECK(uvc_device_init());

    /* 主任务订阅 TWDT（固件基线规范：不允许裸奔主循环） */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
