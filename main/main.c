/*
 * ESP32 录音到 SD 卡（WAV 格式，16bit 单声道 PCM）
 * 录好的文件可通过局域网 FTP 无线访问（见下面"无线访问"配置）
 *
 * 支持 3 种麦克风，改下面 MIC_TYPE 一行即可：
 *   MIC_I2S_STD : I2S 数字硅麦，如 INMP441 / SPH0645 / MSM261S4030 / ICS-43434
 *   MIC_PDM     : PDM 数字硅麦，如 MP34DT01 / IM69D130 / SPK0415HM4H
 *   MIC_ADC     : 模拟输出麦克风，如 MAX9814 / MAX4466 / 模拟硅麦放大板
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_adc/adc_continuous.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "wifi_sta.h"
#include "ftp_server.h"
#include "http_server.h"
#include "wifi_cfg.h"
#include "oled.h"

static const char *TAG = "recorder";

/* ============================ 用户配置区 ============================ */

#define MIC_I2S_STD  1
#define MIC_PDM      2
#define MIC_ADC      3

/* 你的麦克风类型 */
#define MIC_TYPE     MIC_I2S_STD

/* 录音参数 */
#define RECORD_MAX_SECONDS  600     // 单段最长时长（秒），防止忘了按停止把卡写满；0 = 不限时长
#define RECORD_BUF_SAMPLES  1024    // 每批读取的采样点数

/* SD 卡 SPI 引脚（VSPI 默认脚，可改） */
#define SD_PIN_CS     5
#define SD_PIN_MOSI   23
#define SD_PIN_MISO   19
#define SD_PIN_SCLK   18
#define SD_MOUNT_POINT  "/sdcard"

/* 麦克风引脚 */
#define MIC_PIN_BCLK  26            // I2S: BCLK(SCK)   PDM: CLK
#define MIC_PIN_WS    25            // I2S: WS(LRCLK)   PDM: 不用
#define MIC_PIN_DIN   22            // I2S: SD(数据出)  PDM: DAT
#define MIC_ADC_UNIT      ADC_UNIT_1
#define MIC_ADC_CHANNEL   ADC_CHANNEL_6   // GPIO34（ADC1_CH6）

/* BOOT 键（GPIO0）：按一下开始录音，录音中再按一下停止并保存 */
#define KEY_GPIO      GPIO_NUM_0

/* 状态指示灯（双灯，高电平点亮）
 *   LED1 GPIO17 系统灯：常亮 = 可录音（空闲待机）；熄灭 = 存储异常
 *   LED2 GPIO16 录音灯：闪烁 = 正在录音；熄灭 = 空闲
 * 若 LED 阴极接引脚（低电平点亮），把 LED_ACTIVE_LEVEL 改成 0
 */
#define LED1_GPIO            GPIO_NUM_17
#define LED2_GPIO            GPIO_NUM_16
#define LED_ACTIVE_LEVEL     1
#define LED_BLINK_MS         500    // 闪烁半周期（毫秒）

/* 录音过程中每隔多少秒把数据刷到卡上并回填 WAV 头（防止突然断电/拔卡产生坏文件） */
#define RECORD_SYNC_SECONDS  5

/* 数字麦克风增益：1=原始，2/4/8=放大（声音太小就调大，爆音就调小） */
#define MIC_DIGITAL_GAIN  1

/* 无线访问（WiFi 名称/密码、FTP 账号）统一在 config.h 里配置 */

/* =================================================================== */

#if MIC_TYPE == MIC_ADC
#define SAMPLE_RATE  8000
#else
#define SAMPLE_RATE  16000
#endif

#define WAV_HEADER_BYTES  44

typedef struct {
    char     riff_id[4];        // "RIFF"
    uint32_t riff_size;         // 36 + data_size
    char     wave_id[4];        // "WAVE"
    char     fmt_id[4];         // "fmt "
    uint32_t fmt_size;          // 16
    uint16_t audio_format;      // 1 = PCM
    uint16_t num_channels;      // 1
    uint32_t sample_rate;
    uint32_t byte_rate;         // sample_rate * channels * bits/8
    uint16_t block_align;       // channels * bits/8
    uint16_t bits_per_sample;   // 16
    char     data_id[4];        // "data"
    uint32_t data_size;
} wav_header_t;

static i2s_chan_handle_t     s_rx_chan = NULL;
#if MIC_TYPE == MIC_ADC
static adc_continuous_handle_t s_adc_handle = NULL;
#endif
static sdmmc_card_t         *s_card = NULL;
static sdmmc_host_t          s_host = SDSPI_HOST_DEFAULT();

/* LED 状态：由 record_task 设置，led_task 执行 */
typedef enum {
    LED_MODE_OFF = 0,       // 不可录音：熄灭
    LED_MODE_ON,            // 可录音（空闲待机）：常亮
    LED_MODE_BLINK,         // 正在录音：闪烁
} led_mode_t;

static volatile led_mode_t s_led_mode = LED_MODE_OFF;
static volatile bool       s_storage_ok = false;   // 存储是否可用（不可用则按键无效）

/* ---------------------------- WAV 头 ---------------------------- */

static wav_header_t make_wav_header(uint32_t data_bytes)
{
    wav_header_t h = {
        .riff_id         = {'R', 'I', 'F', 'F'},
        .riff_size       = 36 + data_bytes,
        .wave_id         = {'W', 'A', 'V', 'E'},
        .fmt_id          = {'f', 'm', 't', ' '},
        .fmt_size        = 16,
        .audio_format    = 1,
        .num_channels    = 1,
        .sample_rate     = SAMPLE_RATE,
        .byte_rate       = SAMPLE_RATE * 1 * (16 / 8),
        .block_align     = 1 * (16 / 8),
        .bits_per_sample = 16,
        .data_id         = {'d', 'a', 't', 'a'},
        .data_size       = data_bytes,
    };
    return h;
}

/* ---------------------------- LED 指示 ---------------------------- */

static void led_write(int idx, bool on)
{
    gpio_num_t pin = (idx == 1) ? LED1_GPIO : LED2_GPIO;
    gpio_set_level(pin, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED1_GPIO) | (1ULL << LED2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    led_write(1, false);                // 上电先熄灭 = "还不能录音"
    led_write(2, false);
}

/* LED 任务：LED1 反映系统状态，LED2 反映录音状态；配置模式下 LED1 快闪 */
static void led_task(void *arg)
{
    bool blink_on = false;

    while (1) {
        if (wifi_cfg_is_active()) {
            /* WiFi 配置模式：LED1 快闪提示，LED2 熄灭 */
            blink_on = !blink_on;
            led_write(1, blink_on);
            led_write(2, false);
        } else {
            switch (s_led_mode) {
            case LED_MODE_ON:
                led_write(1, true);
                led_write(2, false);
                break;

            case LED_MODE_BLINK:
                blink_on = !blink_on;
                led_write(1, true);
                led_write(2, blink_on);
                break;

            case LED_MODE_OFF:
            default:
                led_write(1, false);
                led_write(2, false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_MS));
    }
}

/* ---------------------------- SD 卡挂载 ---------------------------- */

/*
 * SD 卡频率策略：
 *   1) 先用 400 kHz 握手挂载 —— 劣质卡 / 长线只有这个频率能握上
 *   2) 挂载成功后逐档提速，每档必须「读 3 次扇区全过」才算稳，否则退回上一档
 * 这样就不会出现"挂上了却被强行拉到跑不动的高频，然后读写全 CRC 报错"的情况
 */
static const int SD_FREQ_STEPS[] = {4000, 10000, 20000};

/* 把卡时钟设到 freq_khz，并读扇区校验；失败自动退回 fallback_khz */
static bool try_card_freq(int freq_khz, int fallback_khz)
{
    esp_err_t err = sdspi_host_set_card_clk(s_host.slot, freq_khz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 %d kHz 失败: %s", freq_khz, esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (buf == NULL) {
        return false;
    }

    bool ok = false;
    for (int retry = 0; retry < 3; retry++) {
        if (sdmmc_read_sectors(s_card, buf, 0, 1) == ESP_OK) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(buf);

    if (!ok) {
        ESP_LOGW(TAG, "%d kHz 读写校验失败（CRC 错误），退回 %d kHz", freq_khz, fallback_khz);
        sdspi_host_set_card_clk(s_host.slot, fallback_khz);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ok;
}

static esp_err_t mount_sdcard(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   // 卡坏了不自动格式化，避免误删数据
        .max_files = 8,                    // 录音 1 个 + FTP 下载/上传/列目录各占句柄
        .allocation_unit_size = 16 * 1024
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = s_host.slot;

    vTaskDelay(pdMS_TO_TICKS(500));        // 等卡电源稳定，劣质卡上电慢

    /* 握手阶段强制最低速 */
    s_host.max_freq_khz = SDMMC_FREQ_PROBING;         // 400 kHz
    ESP_LOGI(TAG, "以 400 kHz 握手挂载 SD 卡...");

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &s_host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "400 kHz 都挂不上: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查：");
        ESP_LOGE(TAG, "  1) MISO(GPIO19) 有没有 10k 上拉到 3.3V（很多裸卡座模块没有）");
        ESP_LOGE(TAG, "  2) 接线是否松动、线是否太长了（建议 <10cm）");
        ESP_LOGE(TAG, "  3) 卡是否 FAT32 格式、供电是否充足（写卡峰值几百 mA）");
        ESP_LOGE(TAG, "  4) 卡本身太旧/损坏：可把 mount_sdcard() 里 format_if_mount_failed 改成 true 自动格式化（会清空数据）");
        return ret;
    }
    ESP_LOGI(TAG, "SD 卡握手成功，开始逐档提速...");

    int stable_freq = SDMMC_FREQ_PROBING;             // 400 kHz 一定能用
    for (int i = 0; i < sizeof(SD_FREQ_STEPS) / sizeof(SD_FREQ_STEPS[0]); i++) {
        if (!try_card_freq(SD_FREQ_STEPS[i], stable_freq)) {
            break;
        }
        stable_freq = SD_FREQ_STEPS[i];
        ESP_LOGI(TAG, "提速到 %d kHz，读写校验通过", stable_freq);
    }

    ESP_LOGI(TAG, "SD 卡最终工作频率: %d kHz", stable_freq);
    if (stable_freq < 4000) {
        ESP_LOGW(TAG, "频率过低（400 kHz），写卡可能跟不上录音速率，建议改善接线后再试");
    } else if (stable_freq < 10000) {
        ESP_LOGW(TAG, "频率偏低（%d kHz），录音勉强够用；建议缩短接线 / MISO 加 10k 上拉以提速", stable_freq);
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

/* ---------------------------- 麦克风初始化 ---------------------------- */

#if MIC_TYPE == MIC_I2S_STD
static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        /* INMP441：24 位数据在 32 位槽位的高 24 位。
           用立体声帧是为了让 BCLK = 64 * fs（INMP441 要求 SCK = 64 * 采样率），
           INMP441 只在 WS 低电平输出（L/R 接地 = 左声道），所以只取左声道，右声道丢弃 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_PIN_BCLK,
            .ws   = MIC_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI(TAG, "I2S 数字麦克风已就绪 @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}
#endif

#if MIC_TYPE == MIC_PDM
static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
#if SOC_I2S_SUPPORTS_PDM2PCM
        /* 左声道 = 麦克风 SEL/LR 引脚接地 */
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
#else
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
#endif
        .gpio_cfg = {
            .clk = MIC_PIN_BCLK,
            .din = MIC_PIN_DIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI(TAG, "PDM 数字麦克风已就绪 @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}
#endif

#if MIC_TYPE == MIC_ADC
static esp_err_t mic_init(void)
{
    adc_continuous_handle_cfg_t adc_cfg = {
        .max_store_buf_size = 2048,
        .conv_frame_size    = 256,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_cfg, &s_adc_handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
    };
    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = MIC_ADC_CHANNEL & 0x7,
        .unit      = MIC_ADC_UNIT,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    dig_cfg.adc_pattern = &pattern;
    dig_cfg.pattern_num = 1;
    ESP_ERROR_CHECK(adc_continuous_config(s_adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(s_adc_handle));
    ESP_LOGI(TAG, "模拟麦克风(ADC) 已就绪 @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}
#endif

/* ---------------------------- 读取采样点 ---------------------------- */
/* 统一接口：读出 int16 PCM，out_samples 返回实际点数 */

static esp_err_t mic_read(int16_t *out, size_t max_samples, size_t *out_samples)
{
    *out_samples = 0;

#if MIC_TYPE == MIC_I2S_STD
    static uint32_t raw[RECORD_BUF_SAMPLES * 2];       // 立体声帧：左 + 右
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(500));
    if (ret != ESP_OK) {
        return ret;
    }
    size_t frames = bytes_read / (2 * sizeof(uint32_t));
    if (frames > max_samples) {
        frames = max_samples;
    }
    for (size_t i = 0; i < frames; i++) {
        int32_t s24 = ((int32_t)raw[i * 2]) >> 8;      // 左声道 = 麦克风（INMP441 L/R 接地）
        int32_t amp = s24 * MIC_DIGITAL_GAIN;
        if (amp > 32767) {
            amp = 32767;
        } else if (amp < -32768) {
            amp = -32768;
        }
        out[i] = (int16_t)amp;
    }
    *out_samples = frames;
    return ESP_OK;

#elif MIC_TYPE == MIC_PDM
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, out, max_samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(500));
    if (ret != ESP_OK) {
        return ret;
    }
    *out_samples = bytes_read / sizeof(int16_t);
    return ESP_OK;

#else /* MIC_ADC */
    uint8_t buf[256];
    uint32_t ret_num = 0;
    esp_err_t ret = adc_continuous_read(s_adc_handle, buf, sizeof(buf), &ret_num, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret_num == 0) {
        return ESP_OK;
    }
    adc_continuous_data_t parsed[sizeof(buf) / SOC_ADC_DIGI_RESULT_BYTES];
    uint32_t num = 0;
    if (adc_continuous_parse_data(s_adc_handle, buf, ret_num, parsed, &num) != ESP_OK) {
        return ESP_FAIL;
    }
    if (num > max_samples) {
        num = max_samples;
    }
    for (uint32_t i = 0; i < num; i++) {
        int32_t v = ((int32_t)parsed[i].raw_data - 2048) * 16;   // 12bit -> 16bit
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        out[i] = (int16_t)v;
    }
    *out_samples = num;
    return ESP_OK;
#endif
}

/* ---------------------------- 按键 ---------------------------- */

/* 空闲时的等待：阻塞直到用户完成一次「按下 → 松开」，全程消抖 */
static void key_wait_click(void)
{
    while (1) {
        while (gpio_get_level(KEY_GPIO) != 0) {     // 等按下
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(50));              // 按下消抖
        if (gpio_get_level(KEY_GPIO) != 0) {
            continue;                              // 只是抖动，忽略
        }
        while (gpio_get_level(KEY_GPIO) == 0) {     // 等松开
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(50));              // 松开消抖
        return;
    }
}

/* 录音过程中的检查：按下就返回 true（不复位、不等松开，由调用者处理） */
static bool key_pressed_now(void)
{
    if (gpio_get_level(KEY_GPIO) != 0) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));                  // 复检消抖
    return gpio_get_level(KEY_GPIO) == 0;
}

/* 等用户把手指抬起来，避免松手瞬间被当成下一次「开始」 */
static void key_wait_release(void)
{
    while (gpio_get_level(KEY_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---------------------------- 录音写文件 ---------------------------- */

/* 扫描 /sdcard 里所有 recNNN.wav，挑一个不会覆盖已有录音的文件名 */
static void next_record_path(char *out, size_t out_len)
{
    int next = 0;

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            int idx = -1;
            /* 只认 rec<数字>.wav，上限 8 位防止 sscanf 溢出 int */
            if (sscanf(ent->d_name, "rec%8d.wav", &idx) == 1 && idx >= next) {
                next = idx + 1;
            }
        }
        closedir(dir);
    }

    /* 再逐个 stat 确认：FTP 上传 / 手工拷进来的同名文件也算占用 */
    while (1) {
        snprintf(out, out_len, SD_MOUNT_POINT "/rec%03d.wav", next);
        struct stat st;
        if (stat(out, &st) != 0) {                  // 文件不存在 -> 可以用
            break;
        }
        next++;
    }
}

/*
 * 回填 WAV 头：把"已写入的数据字节数"写进文件头，并把缓冲真正刷到卡上。
 * 做完这一步，磁盘上的文件此刻就是一个完整可播放的 WAV。
 * 写完头会重新定位到数据末尾，方便继续追加录音数据。
 */
static bool wav_header_flush(FILE *f, uint32_t data_bytes)
{
    wav_header_t header = make_wav_header(data_bytes);

    if (fseek(f, 0, SEEK_SET) != 0) {
        return false;
    }
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        return false;
    }
    if (fflush(f) != 0) {
        return false;
    }
    /* 回到数据末尾，继续往下追加 */
    if (fseek(f, (long)(WAV_HEADER_BYTES + data_bytes), SEEK_SET) != 0) {
        return false;
    }
    return true;
}

static bool record_to_file(const char *path)
{
    static int16_t samples[RECORD_BUF_SAMPLES];   // static：避免占用任务栈

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法创建文件 %s", path);
        return false;
    }

    /* 先写一个空头占位：录音时长未知，数据长度由 wav_header_flush() 周期性回填 */
    if (!wav_header_flush(f, 0)) {
        ESP_LOGE(TAG, "写 WAV 头失败，存储不可用");
        fclose(f);
        return false;
    }

    uint32_t written = 0;
    bool ok = true;
    uint32_t limit_bytes = (RECORD_MAX_SECONDS > 0)
                         ? (uint32_t)SAMPLE_RATE * RECORD_MAX_SECONDS * sizeof(int16_t)
                         : 0xFFFFFFFF;
    TickType_t t_start = xTaskGetTickCount();
    uint32_t last_report = 0;

    while (1) {
        if (written >= limit_bytes) {
            ESP_LOGW(TAG, "已录满 %d 秒（RECORD_MAX_SECONDS），自动停止", RECORD_MAX_SECONDS);
            break;
        }

        size_t n = 0;
        if (mic_read(samples, RECORD_BUF_SAMPLES, &n) != ESP_OK || n == 0) {
            continue;
        }
        uint32_t nb = n * sizeof(int16_t);
        if (written + nb > limit_bytes) {
            nb = limit_bytes - written;
        }
        if (fwrite(samples, 1, nb, f) != nb) {
            ESP_LOGE(TAG, "写卡失败（卡满或卡被拔出？），本段提前结束");
            ok = false;
            break;
        }
        written += nb;

        uint32_t sec_done = written / (SAMPLE_RATE * sizeof(int16_t));
        if (sec_done != last_report) {
            last_report = sec_done;
            oled_set_seconds(sec_done);

            /* 保护措施：每 RECORD_SYNC_SECONDS 秒把数据真正刷进卡并回填文件头，
               这样就算录音途中断电 / 拔卡 / 复位，已经录进去的部分依然是一个
               能正常播放的完整 WAV，而不会留下 0 字节或长度对不上的坏文件 */
            if (sec_done % RECORD_SYNC_SECONDS == 0) {
                if (!wav_header_flush(f, written)) {
                    ESP_LOGE(TAG, "同步到卡失败（卡被拔出或写满？）");
                    ok = false;
                    break;
                }
                ESP_LOGI(TAG, "已录 %lu 秒，已安全写入卡（此刻断电也不会损坏文件）",
                         (unsigned long)sec_done);
            } else {
                ESP_LOGI(TAG, "已录 %lu 秒（再按一次 BOOT 键停止）", (unsigned long)sec_done);
            }
        }

        /* BOOT 键按下：停止录音并保存 */
        if (key_pressed_now()) {
            ESP_LOGI(TAG, "检测到按键，停止录音，正在保存...");
            key_wait_release();
            break;
        }
    }

    /* 收尾：不管是正常停止、录满上限还是出错退出，都回填真实长度并关闭文件，
       保证落到卡上的文件永远是可播放的完整 WAV */
    if (!wav_header_flush(f, written)) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }

    if (written == 0) {
        ok = false;
    }

    ESP_LOGI(TAG, "完成: %s (%lu 字节, 时长 %lu 秒, 耗时 %lu ms)%s",
             path, (unsigned long)(written + WAV_HEADER_BYTES),
             (unsigned long)(written / (SAMPLE_RATE * sizeof(int16_t))),
             (unsigned long)((xTaskGetTickCount() - t_start) * portTICK_PERIOD_MS),
             ok ? "" : "  <-- 写入过程中出过错，文件可能不完整");
    return ok;
}

/* ---------------------------- 主流程 ---------------------------- */

static void record_task(void *arg)
{
    char path[64];

    while (1) {
        /* 存储不可用（SD 卡/麦克风有问题）：LED 熄灭，按键完全没反应 */
        if (!s_storage_ok) {
            s_led_mode = LED_MODE_OFF;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 空闲待机：LED 常亮 = 可以开始录 */
        s_led_mode = LED_MODE_ON;
        ESP_LOGI(TAG, "空闲中：按一下 BOOT 键(GPIO0)开始录音");
        key_wait_click();

        /* 先找一个不会被覆盖的文件名，再开录 */
        next_record_path(path, sizeof(path));
        ESP_LOGI(TAG, "===== 开始录音: %s（再按一次 BOOT 键停止）=====", path);
        oled_set_file(path);
        oled_set_state(OLED_ST_REC);

        s_led_mode = LED_MODE_BLINK;               // 录音中：LED 闪烁
        bool ok = record_to_file(path);

        if (!ok) {
            /* 写卡出错（卡满 / 被拔出 / 损坏）：不再假装能录，LED 熄灭并停用按键 */
            ESP_LOGE(TAG, "存储异常，已停止录音功能（LED 熄灭，按键无效）");
            s_storage_ok = false;
            s_led_mode = LED_MODE_OFF;
            oled_set_state(OLED_ST_ERROR);
            continue;
        }

        s_led_mode = LED_MODE_ON;                  // 录完回到常亮，等下一次按键
        oled_set_state(OLED_ST_READY);
        vTaskDelay(pdMS_TO_TICKS(300));            // 喘口气，让 WiFi / FTP 有机会干活
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 录音到 SD 卡  |  采样率 %d Hz, 16bit 单声道", SAMPLE_RATE);

    /* OLED 尽早上电显示（找不到屏只会打日志，不影响主功能） */
    oled_init_start();

    /* 状态指示灯：上电先熄灭 = "还不能录音"，等 SD/麦克风都就绪才常亮 */
    led_init();
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 4, NULL, 0);

    /* BOOT 键 */
    gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key_cfg));

    /* SD 卡和麦克风先初始化：这两个最吃资源，别让 WiFi 抢占了它们的机会 */
    if (mount_sdcard() != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡不可用：LED 保持熄灭，按键无效（无法录音存储）");
        oled_set_state(OLED_ST_ERROR);
        return;
    }
    if (mic_init() != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败：LED 保持熄灭，按键无效（无法录音存储）");
        oled_set_state(OLED_ST_ERROR);
        return;
    }

    /* 到这里存储和麦克风都好了，才允许录音（LED 常亮 / 按键生效） */
    s_storage_ok = true;
    oled_set_sd_ok(true);
    oled_set_state(OLED_ST_READY);

#if ENABLE_WIFI_FTP
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 连接 WiFi：优先用配置网页保存的凭据（NVS），没有则用 config.h 默认值 */
    char ssid[33] = WIFI_SSID;
    char pass[65] = WIFI_PASSWORD;
    if (wifi_cfg_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "使用已保存的 WiFi 配置: %s", ssid);
    }

    if (wifi_sta_start(ssid, pass)) {
        /* FTP 服务器常驻，WiFi 断线重连后无需重启即可继续用 */
        if (ftp_server_start(SD_MOUNT_POINT) != ESP_OK) {
            ESP_LOGE(TAG, "FTP 服务器启动失败");
        }
        /* HTTP 在线播放服务：浏览器访问 http://<设备IP>/ 即可在线听录音 */
        if (http_server_start(SD_MOUNT_POINT) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP 服务器启动失败");
        }
        /* 长按 GPIO33 按键可进入 WiFi 配置模式（热点 HIBO_config） */
        wifi_cfg_button_start();
    }
#endif

    xTaskCreatePinnedToCore(record_task, "record", 8192, NULL, 5, NULL, 1);
}
