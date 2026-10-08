idf_component_register(SRCS "main.c" "camera_driver.c" "status_led.c" "flash_led.c" "config_manager.c" "frame_broadcaster.c" "wifi_manager.c" "time_sync.c" "web_server.c" "web_server_util.c" "web_server_static.c" "web_api_system.c" "web_api_config.c" "web_api_camera.c" "web_api_ai.c" "mjpeg_streamer.c" "rtsp_server.cpp" "ai_pipeline.cpp" "onvif_port.c" "at_command.c" "at_port.c" "ota_updater.c" "device_id.c" "csi_motion.cpp" "wifi_channel_health.c" "watchdog.c" "flash_viewers.c"
                    INCLUDE_DIRS "."
                    REQUIRES onvif-c esp-dl human_face_detect quirc nvs_flash spiffs
                    esp_driver_gpio esp_driver_uart esp_driver_tsens esp_wifi esp_netif esp_event esp_http_server espressif__cjson mdns
                    espp__rtsp esp_https_ota esp_http_client app_update esp_partition espectre)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AI_FACE_DETECT_ENABLED=1)
