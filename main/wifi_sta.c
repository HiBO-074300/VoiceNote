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
#include "oled.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_evt_group = NULL;
#define WIFI_GOT_IP_BIT   BIT0

static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动，正在连接 %s ...", (const char *)arg);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
            s_retry_count++;
            ESP_LOGW(TAG, "连接失败 reason=%d，第 %d 次重试", evt->reason, s_retry_count);
            esp_wifi_connect();          // 自动重连，不需要外部干预
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        ESP_LOGI(TAG, "===== WiFi 已连接 =====");
        ESP_LOGI(TAG, "  IP      : " IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "  网关    : " IPSTR, IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "  子网掩码: " IPSTR, IP2STR(&evt->ip_info.netmask));
        ESP_LOGI(TAG, "  FTP 访问: ftp://" IPSTR "  (端口 21)", IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "=======================");
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        oled_set_ip(ip_str);
        oled_set_wifi_ok(true);
        xEventGroupSetBits(s_evt_group, WIFI_GOT_IP_BIT);
    }
}

bool wifi_sta_start(const char *ssid, const char *password)
{
    static char s_ssid[33];

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

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
                                        &wifi_event_handler, s_ssid, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    /* 不强制某种加密方式，避免 WPA/WPA2/WPA3 不匹配导致连不上 */
    cfg.sta.threshold.authmode = 0;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.failure_retry_cnt = 0;       // 0 = 不断重试

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "写入 WiFi 配置失败");
        return false;
    }

    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败");
        return false;
    }

    ESP_LOGI(TAG, "正在连接 \"%s\"（后台自动重连，连上后会打印 IP）", ssid);
    return true;
}
