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
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/i2c_master.h"
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
 */
static void sccb_scan(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CAMERA_PIN_SIOD,
        .scl_io_num = CAMERA_PIN_SIOC,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sccb scan: bus init %s", esp_err_to_name(err));
        return;
    }
    int found = 0;
    char line[80] = "SCCB devices at:";
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, pdMS_TO_TICKS(20)) == ESP_OK) {
            char one[10];
            snprintf(one, sizeof(one), " 0x%02X", addr);
            strncat(line, one, sizeof(line) - strlen(line) - 1);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "SCCB scan: NO device answered — module not seated / wrong orientation / no power");
    } else {
        ESP_LOGI(TAG, "%s", line);
        /* 常见传感器 7bit 地址：0x21/0x30=OV2640 系, 0x3C=OV5640/OV3660/NT99141,
         * 0x1E=GC0308/GC2145, 0x10=OV7670, 0x3D=SC 系列, 0x2D=BF 系列 */
    }
    i2c_del_master_bus(bus);
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
        .pin_xclk     = CAMERA_PIN_XCLK,
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
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = pixel_format,
        .frame_size   = frame_size,

        .jpeg_quality = jpeg_quality,
        .fb_count     = fb_count,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };

    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        return ret;
    }

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

    /* 开机传感器自检：先 SCCB 总线扫描（换模块诊断），再驱动级探测。
     * 换摄像头模块即刻在串口日志看到结果，不用等宿主打开摄像头盲猜。
     * 失败只告警不阻断——UVC 照常枚举，宿主打开时 camera_start_cb 会重试。 */
    sccb_scan();
    esp_err_t probe = camera_init(CAMERA_XCLK_FREQ, PIXFORMAT_JPEG,
                                  FRAMESIZE_VGA, 12, CAMERA_FB_COUNT);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "boot camera probe failed: %s — check module seating/orientation",
                 esp_err_to_name(probe));
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
