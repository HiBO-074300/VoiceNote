#pragma once

#include <stdbool.h>

/*
 * WiFi 多组连接
 *
 * 依序尝试 NVS 中已存的 WiFi（最多 3 组，见 wifi_cfg.h），
 * 若一组都没存则使用 config.h 里的编译默认值。
 * 共尝试 2 轮（WIFI_ROUNDS），全部失败返回 false，
 * 上层应据此进入 WiFi 配置模式（wifi_cfg_enter_config）。
 * 任一组成功即返回 true 并保持连接，断线自动重连该组。
 */
bool wifi_sta_connect(void);
