/*
 * SSD1306 / SH1106 OLED 显示驱动（I2C, 128x64）
 *
 * - 不用 esp_lcd 面板驱动，直接用 i2c_master 手写初始化与逐页写入：
 *   每一行（页）先设页地址 0xB0|p 和列地址，再写 128 字节。
 *   这是 SSD1306 和 SH1106 都支持的最保守写法（esp_lcd 用整屏水平
 *   寻址一次写 1KB，部分兼容屏控制器不认，会把数据堆在一起，表现为
 *   开头几个字符正常、后面全部叠在一起）。
 * - 8x8 ASCII 字体（font8x8_basic，公有领域，行格式：每字节 bit7 = 最左
 *   像素），渲染时逐位转置映射进 SSD1306 页格式帧缓冲（bit y = 页内第 y 行）
 * - 按需刷新：内容变化才写屏，空闲时完全不动，无闪动无重影
 * - SH1106 显存 132 列宽：若整屏内容右移 2 像素，把 OLED_COL_OFFSET 改 2
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "oled.h"

#define OLED_SDA         GPIO_NUM_21
#define OLED_SCL         GPIO_NUM_27
#define OLED_FREQ_HZ     (400 * 1000)
#define OLED_W           128
#define OLED_H           64
#define OLED_PAGES       (OLED_H / 8)            /* 8 页 = 8 文本行 */
#define OLED_COLS        (OLED_W / 8)            /* 8x8 字体：每行 16 字符 */
#define OLED_FB_SIZE     (OLED_W * OLED_H / 8)   /* 1024 字节页格式 */
#define OLED_ROTATE_180  1                       /* 屏幕方向不对就置 0 */
#define OLED_COL_OFFSET  0                       /* SH1106 整体右移 2px 时改 2 */

static const char *TAG = "oled";

/* font8x8_basic（公有领域），收录 0x20(' ') ~ 0x5A('Z')，LSB = 最左像素 */
static const uint8_t FONT8X8[0x5B - 0x20][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, // '!'
    { 0x36, 0x36, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '"'
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, // '#'
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 }, // '$'
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 }, // '%'
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 }, // '&'
    { 0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '''
    { 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 }, // '('
    { 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 }, // ')'
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, // '*'
    { 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 }, // '+'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x0C }, // ','
    { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 }, // '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 }, // '.'
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, // '/'
    { 0x3E, 0x63, 0x63, 0x6B, 0x63, 0x63, 0x3E, 0x00 }, // '0'
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, // '1'
    { 0x3E, 0x63, 0x07, 0x1E, 0x3C, 0x70, 0x7F, 0x00 }, // '2'
    { 0x3E, 0x63, 0x03, 0x1E, 0x03, 0x63, 0x3E, 0x00 }, // '3'
    { 0x06, 0x0E, 0x1E, 0x36, 0x7F, 0x06, 0x06, 0x00 }, // '4'
    { 0x7F, 0x60, 0x7E, 0x03, 0x03, 0x63, 0x3E, 0x00 }, // '5'
    { 0x1E, 0x30, 0x60, 0x7E, 0x63, 0x63, 0x3E, 0x00 }, // '6'
    { 0x7F, 0x63, 0x03, 0x06, 0x0C, 0x0C, 0x0C, 0x00 }, // '7'
    { 0x3E, 0x63, 0x63, 0x3E, 0x63, 0x63, 0x3E, 0x00 }, // '8'
    { 0x3E, 0x63, 0x63, 0x3F, 0x03, 0x06, 0x3C, 0x00 }, // '9'
    { 0x00, 0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00 }, // ':'
    { 0x00, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00 }, // ';'
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, // '<'
    { 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 }, // '='
    { 0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00 }, // '>'
    { 0x3E, 0x63, 0x03, 0x0E, 0x0C, 0x00, 0x0C, 0x00 }, // '?'
    { 0x1E, 0x33, 0x33, 0x3B, 0x3B, 0x1E, 0x03, 0x00 }, // '@'
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, // 'A'
    { 0x3E, 0x63, 0x63, 0x3E, 0x63, 0x63, 0x3E, 0x00 }, // 'B'
    { 0x1C, 0x36, 0x63, 0x60, 0x60, 0x63, 0x36, 0x00 }, // 'C'
    { 0x3E, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00 }, // 'D'
    { 0x7E, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x7E, 0x00 }, // 'E'
    { 0x7E, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x60, 0x00 }, // 'F'
    { 0x1E, 0x30, 0x60, 0x6F, 0x63, 0x63, 0x3F, 0x00 }, // 'G'
    { 0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x00 }, // 'H'
    { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 }, // 'I'
    { 0x07, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00 }, // 'J'
    { 0x63, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x63, 0x00 }, // 'K'
    { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 }, // 'L'
    { 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00 }, // 'M'
    { 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x63, 0x63, 0x00 }, // 'N'
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, // 'O'
    { 0x3E, 0x63, 0x63, 0x3E, 0x60, 0x60, 0x60, 0x00 }, // 'P'
    { 0x1C, 0x36, 0x63, 0x63, 0x67, 0x3E, 0x1C, 0x0E }, // 'Q'
    { 0x3E, 0x63, 0x63, 0x3E, 0x36, 0x63, 0x63, 0x00 }, // 'R'
    { 0x1E, 0x30, 0x60, 0x1E, 0x06, 0x0C, 0x78, 0x00 }, // 'S'
    { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 }, // 'T'
    { 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00 }, // 'U'
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // 'V'
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, // 'W'
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, // 'X'
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x18, 0x00 }, // 'Y'
    { 0x7F, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x7F, 0x00 }, // 'Z'
};

/* 显示状态（各任务写入，oled 任务读取） */
typedef struct {
    oled_state_t state;
    bool         sd_ok;
    bool         wifi_ok;
    uint32_t     seconds;
    char         file[16];
    char         ip[16];
} oled_info_t;

static uint8_t      s_fb[OLED_FB_SIZE];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static oled_info_t  s_info;
static volatile bool s_dirty = true;        /* 内容有变化才重画 */

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_pkt[1 + OLED_W];           /* 控制字节 + 一页数据 */

/* ---------------------------- 状态上报接口 ---------------------------- */

void oled_set_state(oled_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_info.state = st;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void oled_set_sd_ok(bool ok)
{
    portENTER_CRITICAL(&s_mux);
    s_info.sd_ok = ok;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void oled_set_wifi_ok(bool ok)
{
    portENTER_CRITICAL(&s_mux);
    s_info.wifi_ok = ok;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void oled_set_ip(const char *ip)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_info.ip, ip ? ip : "", sizeof(s_info.ip) - 1);
    s_info.ip[sizeof(s_info.ip) - 1] = '\0';
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void oled_set_file(const char *filename)
{
    const char *p = strrchr(filename ? filename : "", '/');
    p = p ? p + 1 : filename;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_info.file, p ? p : "", sizeof(s_info.file) - 1);
    s_info.file[sizeof(s_info.file) - 1] = '\0';
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void oled_set_seconds(uint32_t sec)
{
    portENTER_CRITICAL(&s_mux);
    s_info.seconds = sec;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

/* ---------------------------- 绘制 ---------------------------- */

/*
 * 在 line 行 col 列画一个字符（自动转大写，超出字库显示 '?'）
 *
 * 帧缓冲是 SSD1306 页格式：s_fb[页*128 + 列]，字节的 bit y = 页内第 y 行像素。
 * 字模是行格式：glyph[y] 的 bit7 = 该行最左像素。
 * 所以要做「行 -> 列」的转置映射：glyph[y] 的 bit(7-x) 写进第 x 列字节的 bit y。
 */
static void draw_char8(int line, int col, char c)
{
    if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    }
    if (c < 0x20 || c > 0x5A) {
        c = '?';
    }
    const uint8_t *glyph = FONT8X8[c - 0x20];
    /* 页格式帧缓冲：s_fb[页*128 + 列]，每页（每行文字）跨度 = OLED_W 字节。
       （此前误用 OLED_W/8=16 做行距，8 行文字全部叠进了开头一百多个字节） */
    uint8_t *base = &s_fb[line * OLED_W + col * 8];
    for (int y = 0; y < 8; y++) {
        uint8_t bits = glyph[y];
        if (bits == 0) {
            continue;
        }
        for (int x = 0; x < 8; x++) {
            if (bits & (0x80 >> x)) {
                base[x] |= (uint8_t)(1 << y);
            }
        }
    }
}

/* 整行写 16 个字符，不足补空格 */
static void draw_str8(int line, const char *s)
{
    int col = 0;
    for (; s && *s && col < OLED_COLS; s++, col++) {
        draw_char8(line, col, *s);
    }
    for (; col < OLED_COLS; col++) {
        draw_char8(line, col, ' ');
    }
}

static void oled_render(void)
{
    oled_info_t v;

    portENTER_CRITICAL(&s_mux);
    v = s_info;
    portEXIT_CRITICAL(&s_mux);

    memset(s_fb, 0, sizeof(s_fb));

    char line[24];

    draw_str8(0, "VoiceNote");

    snprintf(line, sizeof(line), "SD:%s NET:%s",
             v.sd_ok ? "OK" : "--", v.wifi_ok ? "OK" : "--");
    draw_str8(1, line);

    /* 每行仅 16 字符，15 字符的 IP 需整行显示，不加前缀 */
    draw_str8(2, v.ip[0] ? v.ip : "IP --");

    switch (v.state) {
    case OLED_ST_INIT:
        draw_str8(3, "ST:INIT");
        break;
    case OLED_ST_READY:
        draw_str8(3, "ST:IDLE");
        break;
    case OLED_ST_ERROR:
        draw_str8(3, "ST:ERROR");
        break;
    case OLED_ST_CONFIG:
        draw_str8(3, "ST:CONFIG");
        break;
    case OLED_ST_REC:
    default:
        snprintf(line, sizeof(line), "ST:REC %02lu:%02lu",
                 (unsigned long)(v.seconds / 60), (unsigned long)(v.seconds % 60));
        draw_str8(3, line);
        break;
    }

    if (v.file[0]) {
        draw_str8(4, v.file);
    }
}

/* ---------------------------- I2C 底层 ---------------------------- */

static int s_tx_err = 0;                   /* I2C 写入失败计数（打印用） */

/* 发单条命令 */
static void oled_cmd(uint8_t c)
{
    uint8_t buf[2] = { 0x00, c };      /* 控制字节 0x00 = 命令 */
    if (i2c_master_transmit(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
        s_tx_err++;
    }
}

/* 把整屏帧缓冲逐页写入显存：每页先设页地址+列地址，再写 128 字节。
   页寻址 + 每页显式定位，在 SSD1306 和 SH1106 上行为一致 */
static void oled_flush(void)
{
    for (int p = 0; p < OLED_PAGES; p++) {
        oled_cmd(0xB0 | (uint8_t)p);                        /* 页地址 = 第 p 行 */
        oled_cmd(0x00 | (OLED_COL_OFFSET & 0x0F));          /* 列地址低 4 位 */
        oled_cmd(0x10 | ((OLED_COL_OFFSET >> 4) & 0x07));   /* 列地址高 3 位 */
        s_pkt[0] = 0x40;                                    /* 控制字节 0x40 = 数据流 */
        memcpy(&s_pkt[1], &s_fb[p * OLED_W], OLED_W);
        if (i2c_master_transmit(s_dev, s_pkt, sizeof(s_pkt), 100) != ESP_OK) {
            s_tx_err++;
            ESP_LOGE(TAG, "第 %d 页数据写入失败", p);
        }
    }
}

/* 面板初始化（com_pins = 0xDA 命令的参数，用于诊断对比不同配置） */
static void oled_init_panel(uint8_t com_pins)
{
    const uint8_t init_seq[] = {
        0xAE,          /* 显示关 */
        0xD5, 0x80,    /* 时钟分频 */
        0xA8, 0x3F,    /* 复用率 = 64 行 */
        0xD3, 0x00,    /* 显示偏移 = 0 */
        0x40,          /* 起始行 = 0 */
        0x8D, 0x14,    /* 开内部升压 */
        0x20, 0x02,    /* 页寻址模式（SH1106 忽略，其默认即页寻址） */
#if OLED_ROTATE_180
        0xA1,          /* 段重映射（左右翻） */
        0xC8,          /* COM 扫描反向（上下翻）→ 合成 180 度 */
#else
        0xA0, 0xC0,
#endif
        0xDA, com_pins,/* COM 引脚配置：0x12=交替 / 0x02=顺序 */
        0x81, 0xCF,    /* 对比度 */
        0xD9, 0xF1,    /* 预充电 */
        0xDB, 0x40,    /* VCOMH 电压 */
        0xA4,          /* 恢复 RAM 内容显示 */
        0xA6,          /* 正常显示（非反色） */
        0xAF,          /* 显示开 */
    };
    uint8_t cmd_buf[sizeof(init_seq) + 1];
    cmd_buf[0] = 0x00;
    memcpy(&cmd_buf[1], init_seq, sizeof(init_seq));
    i2c_master_transmit(s_dev, cmd_buf, sizeof(cmd_buf), 100);
}

/* 单页亮条（诊断用，当前未调用） */
__attribute__((unused))
static void oled_bar_page(int p)
{
    memset(s_fb, 0, sizeof(s_fb));
    if (p >= 0) {
        memset(&s_fb[p * OLED_W], 0xFF, OLED_W);
    }
    oled_flush();
}

/* ---------------------------- 任务 ---------------------------- */

static void oled_task(void *arg)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败，显示功能停用");
        vTaskDelete(NULL);
        return;
    }

    /* 探测地址：常见的 0x3C，其次 0x3D；上电瞬间可能没就绪，重试几次 */
    int addr = -1;
    for (int i = 0; i < 3 && addr < 0; i++) {
        if (i2c_master_probe(bus, 0x3C, 50) == ESP_OK) {
            addr = 0x3C;
        } else if (i2c_master_probe(bus, 0x3D, 50) == ESP_OK) {
            addr = 0x3D;
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (addr < 0) {
        ESP_LOGE(TAG, "I2C 上没找到 OLED（检查 SDA=21/SCL=27 接线），显示功能停用");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "找到 OLED，地址 0x%02X", addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = OLED_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C 设备挂载失败，显示功能停用");
        vTaskDelete(NULL);
        return;
    }

    /* 正常启动：0x12 配置已经过亮条诊断确认正确 */
    oled_init_panel(0x12);
    memset(s_fb, 0, sizeof(s_fb));
    oled_flush();                      /* 上电清屏 */

    while (1) {
        /* 按需刷新：内容没变就完全不写屏，屏上不会有任何闪动/重影 */
        if (s_dirty) {
            s_dirty = false;
            oled_render();
            oled_flush();
        }
        if (s_tx_err != 0) {
            ESP_LOGE(TAG, "I2C 写入失败 %d 次（检查接线/上拉）", s_tx_err);
            s_tx_err = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void oled_init_start(void)
{
    xTaskCreatePinnedToCore(oled_task, "oled", 4096, NULL, 3, NULL, 0);
}
