/*
 * HTTP 在线播放服务
 *
 * 浏览器访问 http://<设备IP>/ 即可列出并在线播放录音。
 * 录音为 16-bit 单声道 PCM WAV，浏览器原生支持。
 * 文件接口支持 Range 请求（206），音频可拖动进度条。
 */

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "http";

static char s_http_root[32] = "/sdcard";

#define HTTP_SEND_BUF   2048

/* =================================================================== */
/*  工具函数                                                            */
/* =================================================================== */

/* 仅允许 rec*.wav 形式的文件名，防止路径穿越 */
static bool name_safe(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    if (strncasecmp(name, "rec", 3) != 0) return false;
    size_t len = strlen(name);
    if (len < 5 || strcasecmp(name + len - 4, ".wav") != 0) return false;
    return true;
}

static const char *html_head =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>声记 VoiceNote</title>"
    "<style>body{font-family:sans-serif;max-width:640px;margin:16px auto;padding:0 12px;}"
    "h2{color:#345}li{margin:10px 0;list-style:none;border-bottom:1px solid #eee;padding:8px 0;}"
    "audio{width:100%;margin-top:4px;}</style></head><body>"
    "<h2>声记 VoiceNote · 录音列表</h2><ul>";

static const char *html_tail = "</ul></body></html>";

/* =================================================================== */
/*  GET /  录音列表                                                     */
/* =================================================================== */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    httpd_resp_send_chunk(req, html_head, strlen(html_head));

    DIR *dir = opendir(s_http_root);
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!name_safe(ent->d_name)) {
                continue;
            }
            char row[256];
            int n = snprintf(row, sizeof(row),
                             "<li><b>%s</b><br>"
                             "<audio controls preload='none' src='/rec?name=%s'></audio></li>",
                             ent->d_name, ent->d_name);
            if (n > 0 && httpd_resp_send_chunk(req, row, (ssize_t)MIN(n, sizeof(row))) != ESP_OK) {
                break;      /* 客户端断开 */
            }
        }
        closedir(dir);
    } else {
        const char *err = "<li>无法访问存储目录</li>";
        httpd_resp_send_chunk(req, err, strlen(err));
    }

    httpd_resp_send_chunk(req, html_tail, strlen(html_tail));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =================================================================== */
/*  GET /rec?name=rec001.wav  WAV 流式下载                              */
/* =================================================================== */

static esp_err_t wav_handler(httpd_req_t *req)
{
    char query[64] = "";
    char name[32] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_OK;
    }

    if (!name_safe(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", s_http_root, name);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 44) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "empty file");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    /* ---- Range 请求（浏览器拖动进度条时发送） ---- */
    long start = 0;
    long end = fsize - 1;

    char range_hdr[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK &&
        strncasecmp(range_hdr, "bytes=", 6) == 0) {
        long s = -1, e = -1;
        if (sscanf(range_hdr + 6, "%ld-%ld", &s, &e) >= 1 && s >= 0 && s < fsize) {
            start = s;
            end = (e >= 0 && e < fsize) ? e : fsize - 1;
            char cr[96];
            snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, fsize);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "206 Partial Content");
        }
    }

    /* ---- 流式发送（chunked） ---- */
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
        return ESP_OK;
    }

    char *buf = malloc(HTTP_SEND_BUF);
    if (buf == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    long remain = end - start + 1;
    esp_err_t ret = ESP_OK;
    while (remain > 0 && ret == ESP_OK) {
        size_t chunk = (size_t)MIN((long)HTTP_SEND_BUF, remain);
        size_t rd = fread(buf, 1, chunk, f);
        if (rd == 0) {
            break;
        }
        ret = httpd_resp_send_chunk(req, buf, (ssize_t)rd);
        remain -= (long)rd;
    }

    free(buf);
    fclose(f);

    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);    /* 结束 chunked 响应 */
    } else {
        ESP_LOGW(TAG, "客户端中断: %s", name);
    }
    return ESP_OK;
}

/* =================================================================== */
/*  启动                                                                */
/* =================================================================== */

esp_err_t http_server_start(const char *root_path)
{
    if (root_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_http_root, sizeof(s_http_root), "%s", root_path);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 2;    /* ESP32 资源有限，够了 */

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler,
    };
    httpd_uri_t uri_wav = {
        .uri = "/rec", .method = HTTP_GET, .handler = wav_handler,
    };
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_wav);

    ESP_LOGI(TAG, "HTTP 在线播放服务已启动，根目录: %s", s_http_root);
    return ESP_OK;
}
