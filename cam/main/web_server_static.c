/*
 * MiBee Cam v0.1 — Web server (static files)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GET / and catch-all GET wildcard — SPIFFS file serving with
 * gzip negotiation (PIT-038) and 404 peer logging (dual-stack, PIT-056).
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server_internal.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"  /* for stat on SPIFFS files */
#include "lwip/sockets.h"   /* 404 peer 取址：sockaddr_storage 判族（PIT-056） */
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "web_server";

/* ------------------------------------------------------------------ */
/*  GET / — static file serving from SPIFFS                           */
/* ------------------------------------------------------------------ */

esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    char filepath[580];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);

    /* Determine content type */
    const char *type = "text/html";
    const char *ext = strrchr(uri, '.');
    if (ext) {
        if (strcmp(ext, ".css") == 0)          type = "text/css";
        else if (strcmp(ext, ".js") == 0)      type = "application/javascript";
        else if (strcmp(ext, ".png") == 0)     type = "image/png";
        else if (strcmp(ext, ".jpg") == 0 ||
                 strcmp(ext, ".jpeg") == 0)   type = "image/jpeg";
        else if (strcmp(ext, ".ico") == 0)     type = "image/x-icon";
        else if (strcmp(ext, ".svg") == 0)     type = "image/svg+xml";
        else if (strcmp(ext, ".json") == 0)    type = "application/json";
        else if (strcmp(ext, ".html") == 0)    type = "text/html";
    }

    /* gzip 协商（PIT-038，对齐 seeed/ai/luatos）：客户端支持且 SPIFFS 有
     * <path>.gz 时优先发送（tools/compress_ui.py 产物，~4x 缩身）。 */
    char accept_enc[64] = {0};
    bool serving_gz = false;
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc, sizeof(accept_enc)) == ESP_OK &&
        strstr(accept_enc, "gzip") != NULL) {
        char gz_path[600];
        snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);
        struct stat st;
        if (stat(gz_path, &st) == 0) {
            strcpy(filepath, gz_path);
            serving_gz = true;
        }
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        /* 404 带来源对端（issue ai#8：设备侧只记 404 不记 URI/IP，NVR 排障
         * 无法对表）——仅记录，不改变响应语义。
         * 根因修正（2026-09-20 实测 .134）：IDF v6 httpd 会话是 IPv6 双栈
         * socket，IPv4 对端以 v4-mapped 返回（fam=AF_INET6）——按
         * sockaddr_in 解析必得 0.0.0.0。必须 sockaddr_storage 判族。 */
        char peer[INET6_ADDRSTRLEN] = "?";
        int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            const void *addr = NULL;
            if (lwip_getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
                if (ss.ss_family == AF_INET) {
                    addr = &((struct sockaddr_in *)&ss)->sin_addr;
                } else if (ss.ss_family == AF_INET6) {
                    addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
                }
            }
            if (addr) {
                char abuf[INET6_ADDRSTRLEN];
                if (inet_ntop(ss.ss_family, addr, abuf, sizeof(abuf)) != NULL) {
                    const char *p = abuf;
                    if (strncasecmp(p, "::ffff:", 7) == 0) {
                        p += 7;   /* IPv4-mapped → 报 IPv4 文本 */
                    }
                    strlcpy(peer, p, sizeof(peer));
                }
            }
        }
        ESP_LOGW(TAG, "404 %s from %s", uri, peer);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, type);
    if (serving_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
