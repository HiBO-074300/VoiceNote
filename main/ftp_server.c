/*
 * ESP32 极简 FTP 服务器（被动模式 PASV）
 *
 * 把 SD 卡上的文件通过局域网共享出来，Windows 资源管理器 / FileZilla / 安卓文件管理器都能直接连。
 * 根目录锁死在 /sdcard，客户端用 ../ 也逃不出去。
 */

#include "ftp_server.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

static const char *TAG = "ftp";

#define FTP_PORT            21
#define FTP_MAX_CLIENTS     3
#define FTP_SESSION_STACK   6144
#define FTP_SESSION_PRIO    4            /* 低于录音任务(5)，保证录音不被抢占录音优先级 */
#define FTP_LINE_BUF        320
#define FTP_DATA_BUF        4096
#define FTP_MAX_PATH        300
/* 拼接中间量留双倍空间，避免编译器判定可能截断 */
#define FTP_PATH_BUF        (FTP_MAX_PATH * 2)
#define FTP_CTRL_TIMEOUT_S  300          /* 控制连接空闲超时 */
#define FTP_DATA_TIMEOUT_S  15

/* FTP 登录账号密码：见 config.h（把密码留空 = 匿名免密登录） */
#ifndef FTP_USERNAME
#define FTP_USERNAME  "esp32"
#endif
#ifndef FTP_PASSWORD
#define FTP_PASSWORD  "esp32"
#endif

static char s_ftp_root[32] = "/sdcard";
static volatile int s_client_count = 0;

typedef struct {
    int  ctrl;                           /* 控制连接 socket */
    int  pasv_listen;                    /* PASV 监听 socket，-1 = 未开启 */
    int  data;                           /* 已建立的数据连接 socket */
    char root[32];
    char cwd[FTP_MAX_PATH];
    bool auth_ok;
    bool should_quit;
    char user[64];
    char rename_from[FTP_MAX_PATH];
    struct sockaddr_in port_addr;        /* PORT 主动模式地址 */
    bool use_port;
} ftp_session_t;

/* ---------------------------- 基础收发 ---------------------------- */

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* 发一行 FTP 响应，自动补 \r\n；支持多行（在 fmt 里手写 \r\n） */
static void ftp_send(ftp_session_t *s, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = strlen(buf);
    if (len + 3 <= sizeof(buf)) {
        buf[len] = '\r';
        buf[len + 1] = '\n';
        buf[len + 2] = '\0';
    }
    send_all(s->ctrl, buf, strlen(buf));
}

/* 读一行命令（\r\n 结尾），返回 0 成功，-1 失败/超时 */
static int read_line(ftp_session_t *s, char *buf, size_t max)
{
    size_t idx = 0;
    for (;;) {
        char c;
        int r = recv(s->ctrl, &c, 1, 0);
        if (r <= 0) {
            return -1;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            buf[idx] = '\0';
            return 0;
        }
        if (idx + 1 < max) {
            buf[idx++] = c;
        }
    }
}

/* ---------------------------- 路径安全 ---------------------------- */

/* 把客户端给的虚拟路径解析成绝对路径，并保证结果始终在 root 之内 */
static bool path_resolve(ftp_session_t *s, const char *arg, char *out, size_t out_sz)
{
    char input[FTP_PATH_BUF];

    if (arg == NULL || arg[0] == '\0') {
        snprintf(input, sizeof(input), "%s", s->cwd);
    } else if (arg[0] == '/') {
        snprintf(input, sizeof(input), "%s%s", s->root, arg);
    } else {
        snprintf(input, sizeof(input), "%s/%s", s->cwd, arg);
    }

    char *p = input;
    char res[FTP_PATH_BUF];
    res[0] = '\0';

    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        char *seg = p;
        while (*p && *p != '/') {
            p++;
        }
        if (*p) {
            *p = '\0';
            p++;
        }

        if (strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            /* 退到 root 就停住，禁止越界 */
            char *slash = strrchr(res, '/');
            if (slash && (size_t)(slash - res) >= strlen(s->root)) {
                *slash = '\0';
            }
            continue;
        }

        size_t need = strlen(res) + strlen(seg) + 2;
        if (need > sizeof(res)) {
            return false;
        }
        strcat(res, "/");
        strcat(res, seg);
    }

    if (res[0] == '\0') {
        snprintf(res, sizeof(res), "%s", s->root);
    }
    if (strncmp(res, s->root, strlen(s->root)) != 0) {
        return false;
    }
    if (strlen(res) + 1 > out_sz) {
        return false;
    }
    strcpy(out, res);
    return true;
}

/* 把绝对路径换算回客户端看到的虚拟路径 */
static const char *to_virtual(const ftp_session_t *s, const char *abs_path)
{
    const char *p = abs_path + strlen(s->root);
    return (p[0] == '\0') ? "/" : p;
}

/* ---------------------------- 数据连接 ---------------------------- */

static void close_pasv(ftp_session_t *s)
{
    if (s->pasv_listen >= 0) {
        close(s->pasv_listen);
        s->pasv_listen = -1;
    }
}

static void close_data(ftp_session_t *s)
{
    if (s->data >= 0) {
        close(s->data);
        s->data = -1;
    }
}

static void set_sock_timeout(int fd, int seconds)
{
    struct timeval tv = {
        .tv_sec = seconds,
        .tv_usec = 0,
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* 按当前模式（PASV / PORT）建立数据连接，返回 socket 或 -1 */
static int data_open(ftp_session_t *s)
{
    struct timeval tv = {
        .tv_sec = FTP_DATA_TIMEOUT_S,
        .tv_usec = 0,
    };

    if (s->use_port) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return -1;
        }
        set_sock_timeout(fd, FTP_DATA_TIMEOUT_S);
        if (connect(fd, (struct sockaddr *)&s->port_addr, sizeof(s->port_addr)) != 0) {
            close(fd);
            return -1;
        }
        s->data = fd;
        return fd;
    }

    if (s->pasv_listen < 0) {
        return -1;
    }
    setsockopt(s->pasv_listen, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int fd = accept(s->pasv_listen, NULL, NULL);
    close_pasv(s);
    if (fd < 0) {
        return -1;
    }
    set_sock_timeout(fd, FTP_DATA_TIMEOUT_S);
    s->data = fd;
    return fd;
}

/* ---------------------------- 具体命令 ---------------------------- */

static const char *skip_list_flags(const char *arg)
{
    if (arg == NULL) {
        return NULL;
    }
    while (*arg == ' ') {
        arg++;
    }
    if (arg[0] == '-') {
        while (*arg && *arg != ' ') {
            arg++;
        }
        while (*arg == ' ') {
            arg++;
        }
    }
    return (arg[0] == '\0') ? NULL : arg;
}

static void cmd_list(ftp_session_t *s, const char *arg, bool long_format)
{
    char dir[FTP_MAX_PATH];
    if (!path_resolve(s, skip_list_flags(arg), dir, sizeof(dir))) {
        ftp_send(s, "550 Bad path");
        return;
    }

    DIR *d = opendir(dir);
    if (d == NULL) {
        ftp_send(s, "550 Failed to open directory");
        return;
    }

    ftp_send(s, "150 Opening ASCII mode data connection for LIST");
    int fd = data_open(s);
    if (fd < 0) {
        ftp_send(s, "425 Can't open data connection");
        closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char full[FTP_PATH_BUF];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        bool is_dir = false;
        unsigned long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (unsigned long)st.st_size;
        }

        char line[FTP_PATH_BUF];
        if (long_format) {
            snprintf(line, sizeof(line), "%s 1 fatfs fatfs %10lu Jan  1 00:00 %s\r\n",
                     is_dir ? "drwxr-xr-x" : "-rw-r--r--", size, ent->d_name);
        } else {
            snprintf(line, sizeof(line), "%s\r\n", ent->d_name);
        }
        if (send_all(fd, line, strlen(line)) != 0) {
            break;
        }
    }

    closedir(d);
    close_data(s);
    ftp_send(s, "226 Transfer complete");
}

static void cmd_retr(ftp_session_t *s, const char *arg)
{
    char path[FTP_MAX_PATH];
    if (!path_resolve(s, arg, path, sizeof(path))) {
        ftp_send(s, "550 Bad path");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ftp_send(s, "550 Failed to open file");
        return;
    }

    ftp_send(s, "150 Opening BINARY mode data connection");
    int fd = data_open(s);
    if (fd < 0) {
        ftp_send(s, "425 Can't open data connection");
        fclose(f);
        return;
    }

    uint8_t *buf = malloc(FTP_DATA_BUF);
    size_t total = 0;
    int blocks = 0;
    if (buf != NULL) {
        size_t n;
        while ((n = fread(buf, 1, FTP_DATA_BUF, f)) > 0) {
            if (send_all(fd, buf, n) != 0) {
                break;
            }
            total += n;
            /* 低速 SD 卡上不要空转占满 CPU，给录音任务让路 */
            if (++blocks % 4 == 0) {
                vTaskDelay(1);
            }
        }
        free(buf);
    }

    fclose(f);
    close_data(s);
    (void)errno;
    ftp_send(s, "226 Transfer complete (%u bytes)", (unsigned)total);
    ESP_LOGI(TAG, "下载 %s (%u 字节)", path, (unsigned)total);
}

static void cmd_stor(ftp_session_t *s, const char *arg)
{
    char path[FTP_MAX_PATH];
    if (!path_resolve(s, arg, path, sizeof(path))) {
        ftp_send(s, "550 Bad path");
        return;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ftp_send(s, "550 Failed to create file");
        return;
    }

    ftp_send(s, "150 Ok to send data");
    int fd = data_open(s);
    if (fd < 0) {
        ftp_send(s, "425 Can't open data connection");
        fclose(f);
        return;
    }

    uint8_t *buf = malloc(FTP_DATA_BUF);
    size_t total = 0;
    int blocks = 0;
    if (buf != NULL) {
        for (;;) {
            int n = recv(fd, buf, FTP_DATA_BUF, 0);
            if (n <= 0) {
                break;
            }
            if (fwrite(buf, 1, n, f) != (size_t)n) {
                ESP_LOGE(TAG, "写 SD 卡失败（卡满/卡被拔出？）");
                break;
            }
            total += n;
            if (++blocks % 4 == 0) {
                vTaskDelay(1);
            }
        }
        free(buf);
    }

    fclose(f);
    close_data(s);
    ftp_send(s, "226 Transfer complete (%u bytes)", (unsigned)total);
    ESP_LOGI(TAG, "上传 %s (%u 字节)", path, (unsigned)total);
}

/* ---------------------------- 命令分发 ---------------------------- */

static bool ftp_handle_line(ftp_session_t *s, char *line)
{
    char cmd[16];
    char *arg = NULL;

    char *sp = strchr(line, ' ');
    if (sp != NULL) {
        *sp = '\0';
        arg = sp + 1;
        while (*arg == ' ') {
            arg++;
        }
    }
    snprintf(cmd, sizeof(cmd), "%s", line);
    for (char *p = cmd; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    /* 登录前的白名单命令 */
    bool need_auth = strcmp(cmd, "USER") && strcmp(cmd, "PASS") && strcmp(cmd, "QUIT") &&
                     strcmp(cmd, "NOOP") && strcmp(cmd, "FEAT") && strcmp(cmd, "SYST") &&
                     strcmp(cmd, "OPTS") && strcmp(cmd, "ABOR") && strcmp(cmd, "TYPE");
    if (need_auth && !s->auth_ok) {
        ftp_send(s, "530 Please login with USER and PASS");
        return true;
    }

    if (strcmp(cmd, "USER") == 0) {
        s->auth_ok = false;
        snprintf(s->user, sizeof(s->user), "%s", arg ? arg : "");
        ftp_send(s, "331 Password required for %s", s->user);

    } else if (strcmp(cmd, "PASS") == 0) {
        bool anonymous = (strlen(FTP_PASSWORD) == 0);
        if (anonymous || (strcmp(s->user, FTP_USERNAME) == 0 && arg && strcmp(arg, FTP_PASSWORD) == 0)) {
            s->auth_ok = true;
            ftp_send(s, "230 Login successful");
            ESP_LOGI(TAG, "客户端登录成功 (user=%s)%s", s->user, anonymous ? " [匿名模式]" : "");
        } else {
            ftp_send(s, "530 Login incorrect");
            ESP_LOGW(TAG, "登录失败 (user=%s)", s->user);
        }

    } else if (strcmp(cmd, "SYST") == 0) {
        ftp_send(s, "215 UNIX Type: L8");

    } else if (strcmp(cmd, "FEAT") == 0) {
        ftp_send(s, "211- Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n211 End");

    } else if (strcmp(cmd, "OPTS") == 0) {
        ftp_send(s, "200 OPTS command successful");

    } else if (strcmp(cmd, "TYPE") == 0) {
        ftp_send(s, "200 Type set to %s", arg ? arg : "I");

    } else if (strcmp(cmd, "NOOP") == 0) {
        ftp_send(s, "200 NOOP ok");

    } else if (strcmp(cmd, "PWD") == 0) {
        ftp_send(s, "257 \"%s\" is the current directory", to_virtual(s, s->cwd));

    } else if (strcmp(cmd, "CWD") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path))) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(s->cwd, sizeof(s->cwd), "%s", path);
                ftp_send(s, "250 Changed to %s", to_virtual(s, s->cwd));
            } else {
                ftp_send(s, "550 No such directory");
            }
        } else {
            ftp_send(s, "550 Bad path");
        }

    } else if (strcmp(cmd, "CDUP") == 0) {
        char *slash = strrchr(s->cwd, '/');
        if (slash && (size_t)(slash - s->cwd) >= strlen(s->root)) {
            *slash = '\0';
        }
        ftp_send(s, "250 Changed to %s", to_virtual(s, s->cwd));

    } else if (strcmp(cmd, "PASV") == 0) {
        close_pasv(s);
        close_data(s);
        int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (lfd < 0) {
            ftp_send(s, "425 Can't open data connection");
            return true;
        }
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_port = 0,                      /* 交给系统随机分配端口 */
        };
        if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 1) != 0) {
            close(lfd);
            ftp_send(s, "425 Can't open data connection");
            return true;
        }
        s->pasv_listen = lfd;
        s->use_port = false;

        /* 本机 IP 从控制连接取 */
        struct sockaddr_storage local;
        socklen_t slen = sizeof(local);
        getsockname(s->ctrl, (struct sockaddr *)&local, &slen);
        uint32_t ip = ntohl(((struct sockaddr_in *)&local)->sin_addr.s_addr);

        struct sockaddr_in dsi;
        socklen_t dlen = sizeof(dsi);
        getsockname(lfd, (struct sockaddr *)&dsi, &dlen);
        uint16_t port = ntohs(dsi.sin_port);

        ftp_send(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%lu,%lu)",
                 (unsigned long)((ip >> 24) & 0xff), (unsigned long)((ip >> 16) & 0xff),
                 (unsigned long)((ip >> 8) & 0xff), (unsigned long)(ip & 0xff),
                 (unsigned long)((port >> 8) & 0xff), (unsigned long)(port & 0xff));

    } else if (strcmp(cmd, "PORT") == 0) {
        int a, b, c, d, p1, p2;
        if (arg && sscanf(arg, "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &p1, &p2) == 6) {
            close_pasv(s);
            close_data(s);
            memset(&s->port_addr, 0, sizeof(s->port_addr));
            s->port_addr.sin_family = AF_INET;
            s->port_addr.sin_port = htons((uint16_t)(p1 * 256 + p2));
            s->port_addr.sin_addr.s_addr = htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                                                 ((uint32_t)c << 8) | (uint32_t)d);
            s->use_port = true;
            ftp_send(s, "200 PORT command successful");
        } else {
            ftp_send(s, "501 Syntax error in parameters");
        }

    } else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
        cmd_list(s, arg, strcmp(cmd, "LIST") == 0);

    } else if (strcmp(cmd, "RETR") == 0) {
        cmd_retr(s, arg);

    } else if (strcmp(cmd, "STOR") == 0) {
        cmd_stor(s, arg);

    } else if (strcmp(cmd, "SIZE") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path))) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                ftp_send(s, "213 %lu", (unsigned long)st.st_size);
            } else {
                ftp_send(s, "550 Could not get file size");
            }
        } else {
            ftp_send(s, "550 Bad path");
        }

    } else if (strcmp(cmd, "MDTM") == 0) {
        /* 板子没有 RTC，时间没意义，给个固定值让客户端不报错 */
        ftp_send(s, "213 19800101000000");

    } else if (strcmp(cmd, "DELE") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path)) && unlink(path) == 0) {
            ftp_send(s, "250 File deleted");
        } else {
            ftp_send(s, "550 Failed to delete file");
        }

    } else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path)) && mkdir(path, 0777) == 0) {
            ftp_send(s, "257 \"%s\" created", arg);
        } else {
            ftp_send(s, "550 Failed to create directory");
        }

    } else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path)) && rmdir(path) == 0) {
            ftp_send(s, "250 Directory removed");
        } else {
            ftp_send(s, "550 Failed to remove directory");
        }

    } else if (strcmp(cmd, "RNFR") == 0) {
        if (path_resolve(s, arg, s->rename_from, sizeof(s->rename_from))) {
            ftp_send(s, "350 Ready for RNTO");
        } else {
            ftp_send(s, "550 Bad path");
        }

    } else if (strcmp(cmd, "RNTO") == 0) {
        char path[FTP_MAX_PATH];
        if (path_resolve(s, arg, path, sizeof(path)) &&
            rename(s->rename_from, path) == 0) {
            ftp_send(s, "250 Renamed");
        } else {
            ftp_send(s, "550 Failed to rename");
        }
        s->rename_from[0] = '\0';

    } else if (strcmp(cmd, "ABOR") == 0) {
        close_data(s);
        close_pasv(s);
        ftp_send(s, "226 Aborted");

    } else if (strcmp(cmd, "QUIT") == 0) {
        ftp_send(s, "221 Goodbye");
        return false;

    } else {
        ftp_send(s, "502 Command \"%s\" not implemented", cmd);
    }

    return true;
}

/* ---------------------------- 会话任务 ---------------------------- */

static void ftp_session_task(void *pv)
{
    ftp_session_t *s = (ftp_session_t *)pv;

    ftp_send(s, "220 ESP32 FTP Server ready.");

    char line[FTP_LINE_BUF];
    while (!s->should_quit) {
        if (read_line(s, line, sizeof(line)) != 0) {
            break;
        }
        if (!ftp_handle_line(s, line)) {
            break;
        }
    }

    close_data(s);
    close_pasv(s);
    shutdown(s->ctrl, SHUT_RDWR);
    close(s->ctrl);

    s_client_count--;
    free(s);
    ESP_LOGI(TAG, "客户端断开（当前连接数 %d）", s_client_count);
    vTaskDelete(NULL);
}

/* ---------------------------- 监听任务 ---------------------------- */

static void ftp_listen_task(void *pv)
{
    (void)pv;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "创建监听 socket 失败");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(FTP_PORT),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "绑定 %d 端口失败", FTP_PORT);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, FTP_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "listen 失败");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "FTP 服务器已启动，端口 %d，根目录 %s", FTP_PORT, s_ftp_root);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int fd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_client_count >= FTP_MAX_CLIENTS) {
            ESP_LOGW(TAG, "连接数已满(%d)，拒绝新客户端", FTP_MAX_CLIENTS);
            shutdown(fd, SHUT_RDWR);
            close(fd);
            continue;
        }

        ftp_session_t *s = calloc(1, sizeof(ftp_session_t));
        if (s == NULL) {
            close(fd);
            continue;
        }

        set_sock_timeout(fd, FTP_CTRL_TIMEOUT_S);
        s->ctrl = fd;
        s->pasv_listen = -1;
        s->data = -1;
        s->use_port = false;
        snprintf(s->root, sizeof(s->root), "%s", s_ftp_root);
        snprintf(s->cwd, sizeof(s->cwd), "%s", s_ftp_root);

        s_client_count++;
        char name[16];
        snprintf(name, sizeof(name), "ftp_%d", fd);
        if (xTaskCreate(ftp_session_task, name, FTP_SESSION_STACK, s, FTP_SESSION_PRIO, NULL) != pdPASS) {
            ESP_LOGE(TAG, "创建会话任务失败");
            s_client_count--;
            free(s);
            close(fd);
            continue;
        }

        char ip_str[32];
        inet_ntoa_r(cli.sin_addr, ip_str, sizeof(ip_str) - 1);
        ESP_LOGI(TAG, "客户端 %s:%d 已连接（当前连接数 %d）",
                 ip_str, ntohs(cli.sin_port), s_client_count);
    }
}

esp_err_t ftp_server_start(const char *root_path)
{
    if (root_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_ftp_root, sizeof(s_ftp_root), "%s", root_path);

    if (xTaskCreatePinnedToCore(ftp_listen_task, "ftp_listen", 4096, NULL,
                                FTP_SESSION_PRIO, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
