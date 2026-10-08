/*
 * MiBee Cam v0.1 — Web server (core)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Route table (registration order), httpd lifecycle,
 * TCP socket tuning. Handler bodies: web_api_*.c / web_server_static.c.
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server.h"
#include "web_server_internal.h"
#include "ota_updater.h"     /* api_ota_* handlers for the route table + FW_VERSION 兜底 */
#include "esp_http_server.h"
#include "lwip/sockets.h"    /* TCP_NODELAY/KEEPALIVE sockopts（PIT-018 教训：WIP 漏 include，2026-09-04 补） */
#include "esp_log.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ */
/*  Route table — the complete HTTP surface of this server, listed in  */
/*  registration order. Handler bodies live further down in this file. */
/*  Wildcard matching is registration-order sensitive: the GET catch-  */
/*  all must stay LAST.                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char    *uri;
    httpd_method_t method;
    esp_err_t    (*handler)(httpd_req_t *);
} uri_entry_t;

static const uri_entry_t s_uris[] = {
    /* Static files */
    { "/",              HTTP_GET,     static_file_handler          },
    /* REST API — 核心端点（契约 v1.0，四板一致） */
    { "/api/status",    HTTP_GET,     api_status_handler           },
    { "/api/config",    HTTP_GET,     api_config_get_handler       },
    { "/api/config",    HTTP_POST,    api_config_post_handler      },
    { "/api/capabilities", HTTP_GET,  api_capabilities_handler     },
    { "/api/capture",   HTTP_GET,     api_capture_handler          },
    { "/api/scan",      HTTP_GET,     api_scan_handler             },
    { "/api/reset",     HTTP_POST,    api_reset_handler            },
    { "/api/reboot",    HTTP_POST,    api_reboot_handler           },
    { "/api/csi/calibrate", HTTP_POST, api_csi_calibrate_handler   },   /* 契约 v1.7 */
    { "/api/time",      HTTP_POST,    api_time_handler             },
    { "/metrics",       HTTP_GET,     metrics_handler              },
    /* 能力门控端点 */
    { "/api/led",       HTTP_POST,    api_led_handler              },
    { "/api/led",       HTTP_GET,     api_led_get_handler          },
    { "/api/ai",        HTTP_POST,    api_ai_handler               },
    { "/api/ai/status", HTTP_GET,     ai_status_get_handler        },
    { "/api/camera",    HTTP_GET,     api_camera_get_handler       },
    { "/api/camera",    HTTP_POST,    api_camera_post_handler      },
    /* OTA（契约 v1.1：与 seeed 同语义） */
    { "/api/ota",       HTTP_POST,    api_ota_handler              },
    { "/api/ota/info",  HTTP_GET,     api_ota_info_handler         },
    { "/api/ota/upload", HTTP_POST,   api_ota_upload_handler       },
    { "/api/ota/spiffs", HTTP_POST,   api_ota_spiffs_handler       },
    /* CORS preflight */
    { "/*",             HTTP_OPTIONS, options_handler              },
    /* Catch-all static files */
    { "/*",             HTTP_GET,     static_file_handler          },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Set TCP_NODELAY + keepalive on every new HTTP connection.
 * NODELAY: disable Nagle's algorithm so small HTTP writes (headers, MJPEG
 * boundaries) aren't delayed by up to 1 RTT on marginal WiFi.
 * KEEPALIVE: detect dead connections in ~11s (5s idle + 3×2s probes),
 * freeing up limited server sockets for new clients. */
static esp_err_t on_session_open(httpd_handle_t hd, int sockfd)
{
    int enable = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    int keepalive = 1;
    int keepidle  = 5;
    int keepintvl = 2;
    int keepcnt   = 3;
    setsockopt(sockfd, SOL_SOCKET,  SO_KEEPALIVE,  &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
    return ESP_OK;
}

esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port        = port;
    config.max_uri_handlers   = NUM_URIS + 4;   /* 2 ONVIF + events_service（v1.5）+ 富余 */
    config.stack_size         = 16384;
    config.max_open_sockets   = 7;
    config.uri_match_fn       = httpd_uri_match_wildcard;
    config.recv_wait_timeout  = 10;
    config.send_wait_timeout  = 10;
    config.keep_alive_enable  = false;
    config.lru_purge_enable   = true;
    config.open_fn            = on_session_open;  /* TCP_NODELAY + keepalive per socket */

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed on port %d: %s",
                 port, esp_err_to_name(ret));
        return ret;
    }

    /* Register all URI handlers */
    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_uri_t uri = {
            .uri      = s_uris[i].uri,
            .method   = s_uris[i].method,
            .handler  = s_uris[i].handler,
            .user_ctx = NULL,
        };
        ret = httpd_register_uri_handler(s_server, &uri);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register %s %s: %s",
                     (s_uris[i].method == HTTP_GET ? "GET" :
                      s_uris[i].method == HTTP_POST ? "POST" : "OPTIONS"),
                     s_uris[i].uri, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Web server started on port %d (%zu handlers)", port, NUM_URIS);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
