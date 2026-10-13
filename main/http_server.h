#pragma once

#include "esp_err.h"

/*
 * HTTP 在线播放服务（基于 esp_http_server，端口 80）
 * - GET /          录音列表网页，浏览器点击即可在线播放
 * - GET /rec?name=rec001.wav   流式返回 WAV 文件，支持 Range（可拖进度条）
 */
esp_err_t http_server_start(const char *root_path);
