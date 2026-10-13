#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * WiFi 配置门户
 *
 * 长按 GPIO33 按键 3 秒进入配置模式：
 *   - 本机切换为热点：SSID = CFG_AP_SSID，密码 = CFG_AP_PASSWORD（见 config.h）
 *   - 手机/电脑连接后浏览器访问 http://192.168.4.1
 *   - 网页上填写新的 WiFi 名称和密码，保存后设备自动重启并连接新 WiFi
 *   - 配置保存在 NVS（命名空间 "wificfg"），断电不丢
 */

/* 从 NVS 读取已保存的 WiFi 配置；从未保存过返回 false（ssid/pass 内容不变） */
bool wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* 创建配置按键监测任务（GPIO33，长按进入配置模式） */
void wifi_cfg_button_start(void);

/* 当前是否处于配置热点模式（LED/OLED 指示用） */
bool wifi_cfg_is_active(void);
