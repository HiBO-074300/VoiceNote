# ESP32 工程编译脚本
# 用法（PowerShell）: .\build.ps1          编译
#                    .\build.ps1 flash    编译并烧录（烧录前改 PORT）
#                    .\build.ps1 monitor  打开串口监视

param(
    [Parameter(Position = 0)]
    [ValidateSet('', 'build', 'flash', 'monitor', 'menuconfig', 'clean', 'fullclean')]
    [string]$Action = 'build',

    [string]$Port = 'COM3'
)

$ErrorActionPreference = 'Stop'

# esptool 输出含希腊字符，GBK 控制台会 UnicodeEncodeError，强制 Python 输出 UTF-8
$env:PYTHONIOENCODING = 'utf-8'

# 激活 ESP-IDF v5.5.4 环境（内部会先把专用 Python 3.11 加入 PATH 再调 export.ps1）
. 'D:\DATA\Espressif\activate-idf.ps1' | Out-Null

Set-Location $PSScriptRoot

switch ($Action) {
    'flash'      { idf.py -p $Port flash }
    'monitor'    { idf.py -p $Port monitor }
    'menuconfig' { idf.py menuconfig }
    'clean'      { idf.py clean }
    'fullclean'  { idf.py fullclean }
    default      { idf.py build }
}
