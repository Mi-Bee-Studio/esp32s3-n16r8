/*
 * MiBee usb-webcam — GOOUUU ESP32-S3-N16R8 camera pin map
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 数据源：cam/ 子项目 sdkconfig.defaults（上板验证）；主板为根规范
 * "项目间不共享代码"——改引脚时与 cam/sdkconfig.defaults 两处同步。
 */
#pragma once

#define CAMERA_MODULE_NAME "GOOUUU-ESP32-S3-N16R8"

#define CAMERA_PIN_PWDN  -1
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK  15
#define CAMERA_PIN_SIOD  4
#define CAMERA_PIN_SIOC  5

#define CAMERA_PIN_D7    16
#define CAMERA_PIN_D6    17
#define CAMERA_PIN_D5    18
#define CAMERA_PIN_D4    12
#define CAMERA_PIN_D3    10
#define CAMERA_PIN_D2    8
#define CAMERA_PIN_D1    9
#define CAMERA_PIN_D0    11
#define CAMERA_PIN_VSYNC 6
#define CAMERA_PIN_HREF  7
#define CAMERA_PIN_PCLK  13
