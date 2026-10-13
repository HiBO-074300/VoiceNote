/*
 * WiFi 配置门户（AP + 内置网页）
 *
 * 长按 GPIO33 进入配置模式：
 *   1. 停掉 STA 连接，切换为热点（SSID/密码见 config.h）
 *   2. 启动 HTTP 服务（80 端口），网关地址固定 192.168.4.1
 *   3. 用户连接热点后访问 http://192.168.4.1，填入新 WiFi 名称/密码
 *   4. 保存到 NVS 后设备自动重启，用新配置连网
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "config.h"
#include "oled.h"
#include "wifi_cfg.h"

static const char *TAG = "wificfg";

static volatile bool s_active = false;   /* 配置模式进行中 */
static char s_cur_ssid[33];              /* 当前生效的 WiFi 名（网页上展示） */

bool wifi_cfg_is_active(void)
{
    return s_active;
}

/* ---------------------------- NVS 存取 ---------------------------- */

static bool nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wificfg", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("wificfg", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l1 = ssid_len, l2 = pass_len;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &l1);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &l2);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

/* ---------------------------- 工具 ---------------------------- */

/* 原地 URL 解码：%XX 十六进制转义 + '+' 转空格 */
static void url_decode(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '%' && s[1] != '\0' && s[2] != '\0') {
            char hex[3] = { s[1], s[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *w++ = ' ';
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

/* ---------------------------- HTTP 网页 ---------------------------- */

static esp_err_t handler_home(httpd_req_t *req)
{
    char page[1024];
    int n = snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset='gb2312'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi 配置</title></head><body style='font-family:sans-serif'>"
        "<h2>SD 录音机 WiFi 配置</h2>"
        "<p>当前 WiFi：<b>%s</b></p>"
        "<form action='/save' method='get'>"
        "<p>新 WiFi 名称：<br>"
        "<input name='ssid' maxlength='32' style='width:100%%'></p>"
        "<p>新 WiFi 密码：<br>"
        "<input name='pass' maxlength='64' style='width:100%%'></p>"
        "<p><input type='submit' value='保存并重启'></p>"
        "</form>"
        "<p style='color:#888'>保存后设备会自动重启并连接新 WiFi，"
        "IP 地址会显示在屏幕上。</p>"
        "</body></html>", s_cur_ssid);
    httpd_resp_set_type(req, "text/html; charset=gb2312");
    httpd_resp_send(req, page, n);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char query[200];
    char val[100];
    char ssid[33] = "";
    char pass[65] = "";

    /* 先取整个 query 串（如 "ssid=xxx&pass=yyy"），再逐个解析键值 */
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "ssid", val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            strncpy(ssid, val, sizeof(ssid) - 1);
        }
        if (httpd_query_key_value(query, "pass", val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            strncpy(pass, val, sizeof(pass) - 1);
        }
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "WiFi 名称不能为空，请返回重填");
        return ESP_OK;
    }

    if (!nvs_save(ssid, pass)) {
        ESP_LOGE(TAG, "NVS 保存失败");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "已保存新 WiFi 配置: %s，设备即将重启", ssid);

    httpd_resp_set_type(req, "text/html; charset=gb2312");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset='gb2312'></head>"
        "<body style='font-family:sans-serif'><h2>已保存！</h2>"
        "<p>设备正在重启并连接新 WiFi，稍等几秒即可。</p></body></html>",
        HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));   /* 让浏览器先收到响应 */
    esp_restart();
    return ESP_OK;                     /* 不会执行到这里 */
}

/* ---------------------------- 配置模式 ---------------------------- */

static void start_config_mode(void)
{
    if (s_active) {
        return;
    }
    s_active = true;

    /* 网页上展示当前 WiFi 名（NVS 有就用，没有用编译时默认值） */
    strncpy(s_cur_ssid, WIFI_SSID, sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
    char tmp_pass[65];
    wifi_cfg_load(s_cur_ssid, sizeof(s_cur_ssid), tmp_pass, sizeof(tmp_pass));

    ESP_LOGI(TAG, "===== 进入 WiFi 配置模式 =====");
    oled_set_ip("192.168.4.1");
    oled_set_state(OLED_ST_CONFIG);

    /* 停 STA，切换为热点 */
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "创建 AP 网卡失败");
        return;
    }

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, CFG_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(CFG_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, CFG_AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 启动配置网页（80 端口） */
    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));
    httpd_uri_t uri_home = { .uri = "/",     .method = HTTP_GET, .handler = handler_home };
    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_GET, .handler = handler_save };
    httpd_register_uri_handler(server, &uri_home);
    httpd_register_uri_handler(server, &uri_save);

    ESP_LOGI(TAG, "热点 \"%s\" 已开启（密码 %s）", CFG_AP_SSID, CFG_AP_PASSWORD);
    ESP_LOGI(TAG, "请连接该热点后访问 http://192.168.4.1 修改 WiFi");

    /* 配置模式常驻运行，保存后由 /save 重启设备 */
    vTaskDelete(NULL);
}

/* ---------------------------- 按键任务 ---------------------------- */

static void button_task(void *arg)
{
    gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << CFG_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key_cfg);

    while (1) {
        if (gpio_get_level(CFG_KEY_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));             /* 按下消抖 */
            if (gpio_get_level(CFG_KEY_GPIO) != 0) {
                continue;
            }
            int ms = 0;
            while (gpio_get_level(CFG_KEY_GPIO) == 0) { /* 持续按住计时 */
                vTaskDelay(pdMS_TO_TICKS(20));
                ms += 20;
            }
            if (ms >= CFG_KEY_LONGPRESS_MS) {
                ESP_LOGW(TAG, "长按 %d ms，进入 WiFi 配置模式", ms);
                start_config_mode();                    /* 内部不返回 */
                continue;
            }
            /* 短按：忽略 */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void wifi_cfg_button_start(void)
{
    xTaskCreatePinnedToCore(button_task, "cfgkey", 3072, NULL, 3, NULL, 0);
}
