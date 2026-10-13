# 声记 VoiceNote — ESP32 语音灵感记录终端

基于 ESP32-WROOM-32 的独立语音记录设备：按键录音到 SD 卡（WAV 格式），OLED 实时显示状态，支持 WiFi 无线访问 —— 可通过 FTP 下载录音，也可在浏览器中**在线播放**。

**定位**：AI 助手的离线灵感采集终端。灵感稍纵即逝——突然爆发的想法、需要记录的重要信息、安排给 AI 的任务，掏出它按一下就能存下来，之后交给 AI 整理归档。

---

## 功能特性

| 功能 | 说明 |
|---|---|
| 录音 | BOOT 键（GPIO0）按一下开始录音，再按一下停止保存，文件名自动查重（`rec000.wav`、`rec001.wav`…） |
| 音频格式 | 16-bit 单声道 PCM WAV，I2S 麦克风 16 kHz 采样（模拟麦 8 kHz） |
| 断电保护 | 每 5 秒刷新数据到卡并回填 WAV 头，突然断电/拔卡不产生坏文件 |
| 单段时长限制 | 默认最长 600 秒（可配置），防止忘停把卡写满 |
| OLED 显示 | SSD1306 128×64（I2C）：SD/网络状态、完整 IP 地址、录音时长、文件名 |
| 双 LED 指示 | LED1（GPIO17）常亮=可录音 / 熄灭=存储异常；LED2（GPIO16）闪烁=录音中 |
| FTP 服务 | 常驻后台，用 WinSCP/FileZilla 等客户端无线下载录音 |
| **在线播放** | 浏览器访问 `http://<设备IP>/`，网页列出所有录音，点击即可在线听，支持拖动进度条 |
| WiFi 配置门户 | 长按 GPIO33 按键 3 秒进入配置模式，手机连热点即可改 WiFi，无需重新烧录 |

---

## 硬件接线

详细说明（含 strapping 引脚警告、ADC 约束、空闲引脚表）见 [引脚分配.md](引脚分配.md)。

| 外设 | 引脚 | 要点 |
|---|---|---|
| SD 卡（SPI） | CS=5, MOSI=23, MISO=19, SCLK=18 | MISO 需 10k 上拉；线 ≤10cm；必须共地 |
| SD 卡插入检测 | GPIO34 | **无内部上拉，需外部 10k 上拉到 3.3V** |
| I2S 麦克风（INMP441） | SCK=26, WS=25, SD=22 | L/R 接 GND；VDD 3.3V；也支持 PDM 麦和模拟麦（ADC） |
| OLED（SSD1306） | SDA=21, SCL=27 | I2C，地址多为 0x3C |
| LED1/LED2 | GPIO17 / GPIO16 | 串 1k 限流电阻，高电平点亮 |
| 配置按键 | GPIO33 | 长按 3 秒进入 WiFi 配置模式 |
| 录音按键 | GPIO0（BOOT 键） | 开始/停止录音 |

---

## 快速开始

### 1. 准备环境

- ESP-IDF **v5.3.1**（已验证；安装时勾选工具链与 Python 3.11）
- 本工程自带构建脚本 `build.ps1`，其中已写死本机的 ESP-IDF 路径：

```powershell
$env:IDF_TOOLS_PATH = 'D:\APP\Espressif'
$env:IDF_PATH = 'D:\APP\Espressif\frameworks\esp-idf-v5.3.1'
```

> 换电脑时把这两行改成实际安装路径即可。串口默认 `COM3`，可用 `-Port` 参数覆盖。

### 2. 配置

**WiFi 与 FTP 账号** — 改 `main/config.h`：

```c
#define WIFI_SSID       "@HiBo"       /* WiFi 名称 */
#define WIFI_PASSWORD   "moshujia"    /* WiFi 密码 */
#define FTP_USERNAME    "esp32"       /* FTP 用户名 */
#define FTP_PASSWORD    "esp32"       /* FTP 密码 */
#define ENABLE_WIFI_FTP 1             /* 0 = 关闭无线，仅本地录音 */
```

**录音与引脚参数** — 改 `main/main.c` 顶部"用户配置区"：

| 宏 | 说明 |
|---|---|
| `MIC_TYPE` | `MIC_I2S_STD`（默认）/ `MIC_PDM` / `MIC_ADC` |
| `RECORD_MAX_SECONDS` | 单段最长秒数，0 = 不限 |
| `MIC_DIGITAL_GAIN` | 数字麦增益 1/2/4/8，声音小调大、爆音调小 |
| `RECORD_SYNC_SECONDS` | 刷卡周期（秒） |

> 注意：若切换为模拟麦（`MIC_ADC`），采样率 8 kHz 低于 ADC DMA 模式 20 kHz 下限，需先调整；GPIO34 已被 SD 卡检测占用，ADC 通道需改为 GPIO35。

### 3. 编译烧录

```powershell
.\build.ps1              # 编译
.\build.bat              # 编译（cmd 批处理版，双击或命令行均可）
.\build.ps1 flash        # 编译并烧录（默认 COM3）
.\build.bat flash COM8   # 编译并烧录（指定串口）
.\build.ps1 monitor      # 串口监视（Ctrl+] 退出）
.\build.ps1 clean        # 清理
.\build.ps1 fullclean    # 彻底清理
```

### 4. 使用

1. 插入 SD 卡，上电；LED1 常亮、OLED 显示 `ST:IDLE` 即就绪
2. 按 BOOT 键开始录音（LED2 闪烁，OLED 显示 `ST:REC 分:秒`），再按停止保存
3. 联网后 OLED 第 2 行显示设备 IP

**在线播放**（推荐）：浏览器访问 `http://<设备IP>/`，点击列表中的播放器即可在线听；点文件名链接可下载原文件。

**FTP 下载**：用 WinSCP / FileZilla 连接 `ftp://<设备IP>`（账号见 `config.h`），注意客户端使用**被动模式**；Windows 资源管理器自带 FTP 支持较差，不建议。

**改 WiFi 配置**：长按 GPIO33 按键 3 秒，设备变为热点 `HIBO_config`（密码 `moshijia`），手机连接后访问 `http://192.168.4.1` 修改，保存后自动重启连接新网络。

---

## 工程结构

```
sd_recorder/
├── CMakeLists.txt          # 顶层构建文件
├── build.ps1               # 一键编译/烧录/监视脚本
├── backup.ps1              # 工程备份脚本
├── new_project.ps1         # 以本工程为模板新建工程的脚本
├── 引脚分配.md             # 引脚分配表与硬件注意事项
├── 使用教程.md             # 面向使用者的操作教程
├── 交接文档.md / 进度.md   # 开发过程文档
└── main/
    ├── config.h            # WiFi/FTP 凭据（唯一需要改密码的地方）
    ├── main.c              # 录音核心（I2S/ADC 采集、WAV 写卡、用户配置区）
    ├── wifi_sta.c          # WiFi STA 连接与断线重连
    ├── wifi_cfg.c          # WiFi 配置门户（AP + 配置网页）
    ├── ftp_server.c        # FTP 服务器（主动/被动模式）
    ├── http_server.c       # HTTP 在线播放服务（录音列表 + WAV 流 + Range）
    └── oled.c              # SSD1306 显示任务（独立低优先级任务，250ms 刷新）
```

## 注意事项

- OLED 每行仅 16 字符（8×8 字体），IP 地址整行显示才能容纳 15 字符的完整地址
- 在线播放与录音共享 SD 卡总线，同时进行时播放可能出现卡顿，重要文件建议 FTP 下载后本地播放
- IRAM 使用率约 84%，是当前最紧张的资源；开启 WiFi 后动态堆剩约 200~250 KB
- GPIO12（MTDI）为 strapping 引脚，上电瞬间不能被拉高，否则 Flash 电压错误无法启动
