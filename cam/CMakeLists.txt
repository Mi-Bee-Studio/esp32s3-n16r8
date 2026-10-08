cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
file(GLOB MIBEE_UI_FILES main/web_ui/*)
# 2026-09-03：目录级依赖不随文件内容编辑触发（build/spiffs.bin 比新 app.js 旧），
# 必须显式 DEPENDS 文件列表，否则固件烧的是旧 UI
# 2026-09-09 PIT-043：压缩进构建。compress_ui.py 原为手工步骤——改 SPA 源文件后忘跑，
# 旧 .gz（gitignored、family_check 不可见）随镜像进固件，设备静默服务旧 UI
# （实例：备用 WiFi 字段不显示）。2026-09-13 修正：gz OUTPUT 显式进 spiffs DEPENDS——
# GLOB 在干净检出上扫不到不存在的 .gz，custom command 成孤儿、压缩静默跳过（PIT-050）。
file(GLOB MIBEE_UI_SRC main/web_ui/*.js main/web_ui/*.html main/web_ui/*.css main/web_ui/*.svg)
set(MIBEE_UI_GZ ${CMAKE_SOURCE_DIR}/main/web_ui/app.js.gz
                ${CMAKE_SOURCE_DIR}/main/web_ui/index.html.gz
                ${CMAKE_SOURCE_DIR}/main/web_ui/style.css.gz
                ${CMAKE_SOURCE_DIR}/main/web_ui/i18n.js.gz
                ${CMAKE_SOURCE_DIR}/main/web_ui/favicon.svg.gz)
add_custom_command(
    OUTPUT ${MIBEE_UI_GZ}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/compress_ui.py
    DEPENDS ${MIBEE_UI_SRC} ${CMAKE_SOURCE_DIR}/tools/compress_ui.py
    COMMENT "gzip web UI (auto, PIT-043)"
    VERBATIM)
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT DEPENDS ${MIBEE_UI_FILES} ${MIBEE_UI_GZ})

# Apply local patch to managed component (managed_components/ is gitignored).
# led_strip 2.5.5 uses MALLOC_CAP_* without including esp_heap_caps.h, which
# fails to compile under IDF v6.0.1. Source-only patch: read at compile time,
# so it applies deterministically on fresh clones and in CI.
set(LED_STRIP_PATCH_DIR ${CMAKE_SOURCE_DIR}/patches/espressif__led_strip)
set(LED_STRIP_MANAGED_DIR ${CMAKE_SOURCE_DIR}/managed_components/espressif__led_strip)
if(EXISTS ${LED_STRIP_MANAGED_DIR}/src/led_strip_spi_dev.c AND EXISTS ${LED_STRIP_PATCH_DIR}/src/led_strip_spi_dev.c)
    file(COPY ${LED_STRIP_PATCH_DIR}/src/led_strip_spi_dev.c DESTINATION ${LED_STRIP_MANAGED_DIR}/src)
    message(STATUS "Applied led_strip patch (esp_heap_caps.h include for IDF v6)")
endif()

# 2026-09-05：espp__task 补丁已回退——洪水真因是 IDLE1 空闲钩子孤儿（PIT-028），
# 该补丁治的是从未启用的守卫路径，属错误归因产物；上游原样 task.cpp 即可。
