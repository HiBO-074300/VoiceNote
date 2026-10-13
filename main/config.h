/*
 * 无线访问配置 WiFi + FTP
 * 这些值是「改这里就能用」的全部凭据，所有源文件都从这里读
 */
#pragma once

/* ---------- WiFi ---------- */
#define WIFI_SSID         "@HiBo"       /* ← 改成你的 WiFi 名称 */
#define WIFI_PASSWORD     "moshujia"     /* ← 改成你的 WiFi 密码（开放网络留空） */

/* ---------- FTP ---------- */
#define FTP_USERNAME      "esp32"           /* FTP 登录用户名 */
#define FTP_PASSWORD      "esp32"           /* FTP 登录密码（留空 "" = 匿名免密登录） */

/* 0 = 关闭无线功能，只做本地录音（可省下 WiFi 占用的几十 KB 内存） */
#define ENABLE_WIFI_FTP   1

/* ---------- WiFi 配置门户（长按 GPIO33 按键进入） ---------- */
/* 进入后本机变成热点，手机连接后浏览器访问 http://192.168.4.1 修改 WiFi */
#define CFG_KEY_GPIO          33             /* 配置按键引脚 */
#define CFG_KEY_LONGPRESS_MS  3000           /* 长按时长（毫秒） */
#define CFG_AP_SSID           "HIBO_config"  /* 配置热点名称 */
#define CFG_AP_PASSWORD       "moshijia"     /* 配置热点密码 */
