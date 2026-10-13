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

# 激活 ESP-IDF v5.3.1 环境（IDF_PATH / 工具链 / Python）
$env:IDF_TOOLS_PATH = 'D:\APP\Espressif'
$env:IDF_PATH = 'D:\APP\Espressif\frameworks\esp-idf-v5.3.1'
. "$env:IDF_PATH\export.ps1"

Set-Location $PSScriptRoot

switch ($Action) {
    'flash'      { idf.py -p $Port flash }
    'monitor'    { idf.py -p $Port monitor }
    'menuconfig' { idf.py menuconfig }
    'clean'      { idf.py clean }
    'fullclean'  { idf.py fullclean }
    default      { idf.py build }
}
