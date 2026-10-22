#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * WiFi 多组配置 + 配置门户
 *
 * 最多存 3 组 WiFi（NVS，命名空间 "wificfg"），连接时按存储顺序尝试。
 * 通过配置网页新增 WiFi 时，新组排到第 1 位，原有组依次后移。
 *
 * 进入配置模式的两种方式：
 *   1. 长按 GPIO33 按键 3 秒（wifi_cfg_button_start 创建的按键任务）
 *   2. 所有已存 WiFi 两轮均连接失败时自动进入（wifi_cfg_enter_config）
 *
 * 配置模式：本机切换为热点（SSID/密码见 config.h），
 * 手机连接后访问 http://192.168.4.1 修改 WiFi，保存后自动重启。
 */

#define WIFI_SLOT_MAX  3    /* 最多存储的 WiFi 组数 */

/* 已存组数（0~3）；兼容旧版单组存储 */
int  wifi_cfg_count(void);

/* 读取第 idx 组（0 起）；不存在返回 false */
bool wifi_cfg_get(int idx, char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* 新增一组：排到第 1 位，原有组后移，超过 3 组时最旧的被挤掉 */
bool wifi_cfg_add(const char *ssid, const char *pass);

/* 兼容旧接口：等同 wifi_cfg_get(0, ...) */
bool wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* 进入 WiFi 配置模式（独立任务运行，可安全从 app_main 调用） */
void wifi_cfg_enter_config(void);

/* 创建配置按键监测任务（GPIO33，长按进入配置模式） */
void wifi_cfg_button_start(void);

/* 当前是否处于配置热点模式（LED/OLED 指示用） */
bool wifi_cfg_is_active(void);
