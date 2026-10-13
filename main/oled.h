#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * SSD1306 OLED 显示（I2C, 128x64）
 * 引脚：SDA=GPIO21, SCL=GPIO27（见 引脚分配.md）
 * 独立低优先级任务刷新，250ms 一次，与录音互不干扰
 */

typedef enum {
    OLED_ST_INIT = 0,       // 上电初始化中
    OLED_ST_READY,          // 空闲，可录音
    OLED_ST_REC,            // 录音中
    OLED_ST_CONFIG,         // WiFi 配置模式（热点开启）
    OLED_ST_ERROR,          // 存储异常
} oled_state_t;

/* 创建 OLED 任务（上电即调用；I2C 上找不到屏则打日志并自动停用） */
void oled_init_start(void);

/* 以下为状态上报接口，任何任务随时可调，线程安全 */
void oled_set_state(oled_state_t st);
void oled_set_sd_ok(bool ok);
void oled_set_wifi_ok(bool ok);
void oled_set_ip(const char *ip);            /* 如 "192.168.1.23" */
void oled_set_file(const char *filename);    /* 传全路径或文件名均可，只取末段 */
void oled_set_seconds(uint32_t sec);         /* 录音已录秒数 */
