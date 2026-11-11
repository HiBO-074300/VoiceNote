/*
 * 自动上传模块：把 SD 卡里的录音同步到电脑上位机。
 *
 * 流程：UDP 广播发现上位机 → TCP 连接 → 逐文件上传（确认后删除）→ 循环
 * 协议（每个文件一帧）：
 *   [4B 魔数 "VNF1"][2B 文件名长度][文件名][4B 文件大小(LE)][文件数据] → 等 "OK"
 *   上位机校验字节数一致回 "OK"（此时才允许删除），不一致回 "NG"（重传）。
 * 安全规则：
 *   - 正在录音的文件（record_file_in_use）绝不读取/上传/删除
 *   - 录音会话进行中（record_busy，含暂停）暂停整个上传流程
 *   - 小于 44 字节（连 WAV 头都不全）的文件视为无效，跳过不删
 */
#include "uploader.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "oled.h"

#define UP_PORT          9001
#define UP_MAGIC         "VNF1"
#define UP_DISC_REQ      "VNOTE_DISCOVER"
#define UP_DISC_RSP      "VNOTE_SERVER"
#define UP_MOUNT         "/sdcard"
#define UP_MAX_FILES     256
#define UP_IO_TIMEOUT_S  10

/* main.c 提供 */
extern bool record_busy(void);
extern bool record_file_in_use(const char *path);
extern bool wifi_cfg_is_active(void);

static const char *TAG = "uploader";

/* ---- 可靠收发小工具 ---- */

static int send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = send(sock, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_exact(int sock, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        int n = recv(sock, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* ---- 单文件传输：返回 0=已确认可删，1=跳过/失败保留，-1=网络故障 ---- */

static int xfer_file(int sock, const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "无法打开 %s", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 44) {                       /* 连 WAV 头都不全，跳过不删 */
        ESP_LOGW(TAG, "跳过无效小文件 %s (%ld B)", path, size);
        fclose(f);
        return 1;
    }

    /* 帧头：魔数 + 文件名 + 大小 */
    size_t nl = strlen(name);
    uint8_t hdr[4 + 2 + 64 + 4];
    size_t off = 0;
    memcpy(hdr + off, UP_MAGIC, 4);                          off += 4;
    hdr[off++] = (uint8_t)(nl & 0xFF);
    hdr[off++] = (uint8_t)((nl >> 8) & 0xFF);
    memcpy(hdr + off, name, nl);                             off += nl;
    uint32_t sz32 = (uint32_t)size;
    hdr[off++] = (uint8_t)(sz32 & 0xFF);
    hdr[off++] = (uint8_t)((sz32 >> 8) & 0xFF);
    hdr[off++] = (uint8_t)((sz32 >> 16) & 0xFF);
    hdr[off++] = (uint8_t)((sz32 >> 24) & 0xFF);

    if (send_all(sock, hdr, off) != 0) {
        fclose(f);
        return -1;
    }

    static uint8_t chunk[4096];            /* 上传任务独享，static 省栈 */
    long remain = size;
    int last_pct = -1;
    while (remain > 0) {
        size_t want = (remain > (long)sizeof(chunk)) ? sizeof(chunk) : (size_t)remain;
        size_t got = fread(chunk, 1, want, f);
        if (got == 0) {
            ESP_LOGE(TAG, "读文件失败 %s", path);
            fclose(f);
            return -1;
        }
        if (send_all(sock, chunk, got) != 0) {
            fclose(f);
            return -1;
        }
        remain -= (long)got;
        int pct = (int)(((size - remain) * 100) / size);
        if (pct != last_pct) {             /* 整数百分比变化才刷屏 */
            oled_set_upload(name, pct);
            last_pct = pct;
        }
    }
    fclose(f);

    /* 等上位机确认：OK 才删 */
    char ack[2] = { 0, 0 };
    if (recv_exact(sock, ack, 2) != 0) {
        return -1;
    }
    if (ack[0] == 'O' && ack[1] == 'K') {
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "已上传并删除 %s (%ld B)", path, size);
            return 0;
        }
        ESP_LOGE(TAG, "删除失败 %s（保留待下次）", path);
        return 1;
    }
    ESP_LOGW(TAG, "上位机校验未通过 %s，稍后重传", path);
    return 1;
}

/* ---- 扫描 /sdcard 下的 rec*.wav，排序后逐个上传 ---- */

static int upload_pass(int sock)
{
    DIR *dir = opendir(UP_MOUNT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "无法打开 " UP_MOUNT);
        return -1;
    }

    static char names[UP_MAX_FILES][32];
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && cnt < UP_MAX_FILES) {
        size_t len = strlen(e->d_name);
        if (len > 6 && len < 32 &&
            strncmp(e->d_name, "rec", 3) == 0 &&
            strcmp(e->d_name + len - 4, ".wav") == 0) {
            memcpy(names[cnt], e->d_name, len + 1);
            cnt++;
        }
    }
    closedir(dir);

    /* 按文件名排序 = 按录制时间从旧到新（插入排序） */
    for (int i = 1; i < cnt; i++) {
        char tmp[32];
        memcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            memmove(names[j + 1], names[j], sizeof(names[0]));
            j--;
        }
        memcpy(names[j + 1], tmp, sizeof(tmp));
    }

    int ok_cnt = 0;
    for (int i = 0; i < cnt; i++) {
        if (record_busy()) {               /* 录音会话开始，本轮立即让路 */
            ESP_LOGI(TAG, "录音进行中，本轮上传终止（下次继续）");
            break;
        }
        char path[64];
        snprintf(path, sizeof(path), UP_MOUNT "/%s", names[i]);
        if (record_file_in_use(path)) {    /* 正在写的文件绝不碰 */
            continue;
        }
        int r = xfer_file(sock, path, names[i]);
        if (r < 0) {
            return -1;                     /* 网络故障，本轮终止 */
        }
        if (r == 0) {
            ok_cnt++;
        }
    }
    ESP_LOGI(TAG, "本轮上传完成：成功 %d / 发现 %d", ok_cnt, cnt);
    return ok_cnt;
}

/* ---- 主任务：发现 → 连接 → 上传 → 循环 ---- */

static void uploader_task(void *arg)
{
    int udp_sock = -1;
    struct sockaddr_in server = { 0 };
    bool have_server = false;

    ESP_LOGI(TAG, "自动上传模块启动（发现/上传端口 %d）", UP_PORT);

    while (1) {
        if (wifi_cfg_is_active()) {        /* 配置热点模式：没有上位机概念 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* 等网络就绪，算本网段广播地址 */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t bc = ip.ip.addr | ~(ip.netmask.addr);

        if (udp_sock < 0) {
            udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (udp_sock < 0) {
                ESP_LOGE(TAG, "UDP socket 创建失败");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            int one = 1;
            setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
            struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
            setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        /* 发现：广播询问，等上位机单播应答（应答源地址就是电脑 IP） */
        if (!have_server && !record_busy()) {
            struct sockaddr_in bc_addr = {
                .sin_family = AF_INET,
                .sin_port   = htons(UP_PORT),
                .sin_addr.s_addr = bc,
            };
            sendto(udp_sock, UP_DISC_REQ, strlen(UP_DISC_REQ), 0,
                   (struct sockaddr *)&bc_addr, sizeof(bc_addr));

            struct sockaddr_in from = { 0 };
            socklen_t fl = sizeof(from);
            char rbuf[16] = { 0 };
            int n = recvfrom(udp_sock, rbuf, sizeof(rbuf) - 1, 0,
                             (struct sockaddr *)&from, &fl);
            if (n > 0 && strncmp(rbuf, UP_DISC_RSP, strlen(UP_DISC_RSP)) == 0) {
                server = from;
                server.sin_port = htons(UP_PORT);
                have_server = true;
                uint32_t pc = ntohl(server.sin_addr.s_addr);
                ESP_LOGI(TAG, "发现上位机: %u.%u.%u.%u:%d",
                         (unsigned)((pc >> 24) & 0xFF), (unsigned)((pc >> 16) & 0xFF),
                         (unsigned)((pc >> 8) & 0xFF), (unsigned)(pc & 0xFF), UP_PORT);
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));   /* 3 秒后再广播 */
                continue;
            }
        }

        /* TCP 连接并传一轮 */
        int ts = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (ts < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        struct timeval tv = { .tv_sec = UP_IO_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(ts, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(ts, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(ts, (struct sockaddr *)&server, sizeof(server)) != 0) {
            ESP_LOGW(TAG, "连接上位机失败，重新发现");
            close(ts);
            have_server = false;           /* 电脑可能下线了，重新走发现 */
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "已连接上位机，开始同步录音");
        oled_set_state(OLED_ST_UPLOAD);    /* 屏幕显示 UP:文件名 进度% */

        int r = upload_pass(ts);
        close(ts);
        oled_set_state(OLED_ST_READY);     /* 传完恢复空闲显示 */

        if (r < 0) {
            have_server = false;           /* 传输中断，重新发现 */
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else {
            /* 刚传完很快可能有新文件，3 秒后再扫；没文件就 30 秒省着点 */
            vTaskDelay(pdMS_TO_TICKS(r > 0 ? 3000 : 30000));
        }
    }
}

void uploader_start(void)
{
    xTaskCreatePinnedToCore(uploader_task, "uploader", 6144, NULL, 3, NULL, 0);
}
