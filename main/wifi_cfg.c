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

/* ---------------------------- NVS 存取（多组） ---------------------------- */

/* 生成索引键名：w0s/w0p, w1s/w1p, w2s/w2p */
static void slot_key(int idx, bool is_pass, char *out, size_t out_len)
{
    snprintf(out, out_len, "w%d%c", idx, is_pass ? 'p' : 's');
}

/* 读出第 idx 组；越界或不存在返回 false */
bool wifi_cfg_get(int idx, char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (idx < 0 || idx >= WIFI_SLOT_MAX) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("wificfg", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[8];
    size_t l1 = ssid_len, l2 = pass_len;
    slot_key(idx, false, key, sizeof(key));
    esp_err_t e1 = nvs_get_str(h, key, ssid, &l1);
    slot_key(idx, true, key, sizeof(key));
    esp_err_t e2 = nvs_get_str(h, key, pass, &l2);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

/* 已存组数（0~3）；兼容旧版单组存储（"ssid"/"pass" 键视为第 1 组） */
int wifi_cfg_count(void)
{
    nvs_handle_t h;
    uint8_t cnt = 0;
    if (nvs_open("wificfg", NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_u8(h, "wcnt", &cnt) != ESP_OK) {
        /* 没有新式计数值：检查旧版键 */
        char tmp[33];
        size_t l = sizeof(tmp);
        cnt = (nvs_get_str(h, "ssid", tmp, &l) == ESP_OK) ? 1 : 0;
    }
    nvs_close(h);
    if (cnt > WIFI_SLOT_MAX) {
        cnt = WIFI_SLOT_MAX;
    }
    return (int)cnt;
}

/* 新增一组 WiFi：新组排第 1 位，原有组依次后移，最多保留 3 组（最旧的被挤掉） */
bool wifi_cfg_add(const char *ssid, const char *pass)
{
    /* 先读出原有的组 */
    char old_s[WIFI_SLOT_MAX][33] = { 0 };
    char old_p[WIFI_SLOT_MAX][65] = { 0 };
    int old_cnt = wifi_cfg_count();
    if (old_cnt > WIFI_SLOT_MAX - 1) {
        old_cnt = WIFI_SLOT_MAX - 1;
    }
    for (int i = 0; i < old_cnt; i++) {
        wifi_cfg_get(i, old_s[i], sizeof(old_s[i]), old_p[i], sizeof(old_p[i]));
    }

    nvs_handle_t h;
    if (nvs_open("wificfg", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    char key[8];
    slot_key(0, false, key, sizeof(key));
    nvs_set_str(h, key, ssid);
    slot_key(0, true, key, sizeof(key));
    nvs_set_str(h, key, pass);
    for (int i = 0; i < old_cnt; i++) {
        slot_key(i + 1, false, key, sizeof(key));
        nvs_set_str(h, key, old_s[i]);
        slot_key(i + 1, true, key, sizeof(key));
        nvs_set_str(h, key, old_p[i]);
    }
    uint8_t new_cnt = (uint8_t)(old_cnt + 1);
    nvs_set_u8(h, "wcnt", new_cnt);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "WiFi 列表已更新，共 %d 组（新组: %s）", (int)new_cnt, ssid);
    return err == ESP_OK;
}

/* 兼容旧接口：读取第 1 组 */
bool wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    return wifi_cfg_get(0, ssid, ssid_len, pass, pass_len);
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
    char page[1536];
    char list[512] = "";

    /* 列出已存的 WiFi 组 */
    int cnt = wifi_cfg_count();
    if (cnt == 0) {
        snprintf(list, sizeof(list), "<li>（无，用编译默认值）</li>");
    }
    for (int i = 0; i < cnt; i++) {
        char ssid[33];
        char pass[65];
        if (wifi_cfg_get(i, ssid, sizeof(ssid), pass, sizeof(pass))) {
            char row[80];
            snprintf(row, sizeof(row), "<li>%d. %s</li>", i + 1, ssid);
            strncat(list, row, sizeof(list) - strlen(list) - 1);
        }
    }

    int n = snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset='gb2312'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi 配置</title></head><body style='font-family:sans-serif'>"
        "<h2>声记 VoiceNote · WiFi 配置</h2>"
        "<p>已存 WiFi（连接时按此顺序尝试，最多 3 组）：</p>"
        "<ol>%s</ol>"
        "<form action='/save' method='get'>"
        "<p>新增 WiFi 名称（将排到第 1 位）：<br>"
        "<input name='ssid' maxlength='32' style='width:100%%'></p>"
        "<p>新增 WiFi 密码：<br>"
        "<input name='pass' maxlength='64' style='width:100%%'></p>"
        "<p><input type='submit' value='保存并重启'></p>"
        "</form>"
        "<p style='color:#888'>保存后设备会自动重启并依次尝试连接，"
        "IP 地址会显示在屏幕上。超过 3 组时最旧的被挤掉。</p>"
        "</body></html>", list);
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

    if (!wifi_cfg_add(ssid, pass)) {
        ESP_LOGE(TAG, "NVS 保存失败");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "已新增 WiFi 配置: %s，设备即将重启", ssid);

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
        vTaskDelete(NULL);
        return;
    }
    s_active = true;

    /* 展示第 1 组已存 WiFi 名（NVS 有就用，没有用编译时默认值） */
    char tmp_pass[65];
    if (!wifi_cfg_get(0, s_cur_ssid, sizeof(s_cur_ssid), tmp_pass, sizeof(tmp_pass))) {
        strncpy(s_cur_ssid, WIFI_SSID, sizeof(s_cur_ssid) - 1);
        s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
    }

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

/* 配置模式入口任务：start_config_mode 末尾 vTaskDelete(NULL)，不会返回 */
static void config_task_entry(void *arg)
{
    start_config_mode();
}

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
                wifi_cfg_enter_config();                /* 内部创建独立任务 */
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

/*
 * 进入 WiFi 配置模式（自动兜底入口）。
 * 在独立任务里运行：start_config_mode 末尾会 vTaskDelete(NULL)，
 * 直接从 app_main 调用会误删主任务，所以包一层。
 */
void wifi_cfg_enter_config(void)
{
    if (s_active) {
        return;
    }
    xTaskCreatePinnedToCore(config_task_entry, "wificfg", 4096, NULL, 3, NULL, 0);
}
