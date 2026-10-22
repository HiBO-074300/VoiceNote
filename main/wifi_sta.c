#include "wifi_sta.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "config.h"
#include "wifi_cfg.h"
#include "oled.h"

static const char *TAG = "wifi";

/* 连接策略：最多存 3 组 WiFi，逐组尝试；两轮全部失败放弃并进入配置模式 */
#define WIFI_ROUNDS          2        /* 总轮数 */
#define WIFI_TRY_TIMEOUT_MS  12000    /* 单组单次尝试超时（含全信道扫描） */

static EventGroupHandle_t s_evt_group = NULL;
#define WIFI_GOT_IP_BIT   BIT0
#define WIFI_FAIL_BIT     BIT1

static volatile bool s_auto_reconnect = false;  /* 连上后断线自动重连 */
static bool s_started = false;                  /* esp_wifi_start 只调一次 */
static char s_cur_ssid[33];                     /* 当前已连上的 WiFi 名 */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* 第一次启动：connect 由 try_connect 在 start 前配置好后再触发 */
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
            if (s_auto_reconnect) {
                /* 已成功连上过：断线自动重连（同一组 WiFi） */
                ESP_LOGW(TAG, "WiFi 断开 reason=%d，自动重连 %s ...", evt->reason, s_cur_ssid);
                esp_wifi_connect();
            } else {
                /* 尝试阶段：失败快速返回，交给上层换下一组 */
                ESP_LOGW(TAG, "连接失败 reason=%d", evt->reason);
                xEventGroupSetBits(s_evt_group, WIFI_FAIL_BIT);
            }
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_auto_reconnect = true;
        ESP_LOGI(TAG, "===== WiFi 已连接 %s =====", s_cur_ssid);
        ESP_LOGI(TAG, "  IP      : " IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "  网关    : " IPSTR, IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "  FTP 访问: ftp://" IPSTR "  (端口 21)", IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "=======================");
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        oled_set_ip(ip_str);
        oled_set_wifi_ok(true);
        xEventGroupSetBits(s_evt_group, WIFI_GOT_IP_BIT);
    }
}

/* 尝试连接一组 WiFi，成功返回 true */
static bool try_connect(const char *ssid, const char *pass, bool first_start)
{
    s_auto_reconnect = false;
    xEventGroupClearBits(s_evt_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    /* 不强制某种加密方式，避免 WPA/WPA2/WPA3 不匹配导致连不上 */
    cfg.sta.threshold.authmode = 0;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.failure_retry_cnt = 1;

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "写入 WiFi 配置失败");
        return false;
    }

    esp_err_t err;
    if (first_start) {
        err = esp_wifi_start();          /* STA_START 事件里自动发起连接 */
    } else {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发起连接失败: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                           WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TRY_TIMEOUT_MS));

    if (bits & WIFI_GOT_IP_BIT) {
        strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
        s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
        s_auto_reconnect = true;         /* 之后断线自动重连本组 */
        return true;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "\"%s\" 连接被拒（密码错误或热点不存在）", ssid);
    } else {
        ESP_LOGW(TAG, "\"%s\" 连接超时", ssid);
    }
    esp_wifi_disconnect();               /* 复位连接状态，准备下一组 */
    return false;
}

/*
 * 依序尝试所有已存的 WiFi（NVS 最多 3 组；若一组都没存则用 config.h 默认值）。
 * 共尝试 WIFI_ROUNDS 轮，全部失败返回 false（上层据此进入配置模式）。
 * 任一组成功即返回 true，并保持连接 + 断线自动重连。
 */
bool wifi_sta_connect(void)
{
    s_evt_group = xEventGroupCreate();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "创建 STA 网卡失败");
        return false;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败");
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);

    /* 收集待尝试的 WiFi 列表 */
    char ssids[WIFI_SLOT_MAX][33];
    char passes[WIFI_SLOT_MAX][65];
    int n = wifi_cfg_count();

    if (n == 0) {
        /* 从未配置过：用编译时默认值兜底 */
        strncpy(ssids[0], WIFI_SSID, sizeof(ssids[0]) - 1);
        strncpy(passes[0], WIFI_PASSWORD, sizeof(passes[0]) - 1);
        n = 1;
        ESP_LOGI(TAG, "NVS 无已存 WiFi，使用编译默认值 \"%s\"", WIFI_SSID);
    } else {
        if (n > WIFI_SLOT_MAX) n = WIFI_SLOT_MAX;
        for (int i = 0; i < n; i++) {
            wifi_cfg_get(i, ssids[i], sizeof(ssids[i]), passes[i], sizeof(passes[i]));
        }
        ESP_LOGI(TAG, "已存 %d 组 WiFi，开始逐组尝试", n);
    }

    oled_set_wifi_ok(false);

    for (int round = 1; round <= WIFI_ROUNDS; round++) {
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "[第 %d/%d 轮] 尝试 \"%s\" ...",
                     round, WIFI_ROUNDS, ssids[i]);
            if (try_connect(ssids[i], passes[i], !s_started)) {
                s_started = true;
                return true;
            }
            s_started = true;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGE(TAG, "%d 轮共 %d 次尝试全部失败", WIFI_ROUNDS, WIFI_ROUNDS * n);
    return false;
}
