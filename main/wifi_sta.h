#pragma once

#include <stdbool.h>

/**
 * @brief 初始化 WiFi Station 模式并连接热点（非阻塞，内部自动重连）
 *
 * 必须先调用 esp_netif_init() 和 esp_event_loop_create_default()
 *
 * @param ssid      WiFi 名称
 * @param password  WiFi 密码（开放网络传 ""）
 * @return true  参数下发成功
 */
bool wifi_sta_start(const char *ssid, const char *password);
