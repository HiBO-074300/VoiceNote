@echo off
rem =====================================================================
rem  声记 VoiceNote - ESP32 编译脚本 (cmd 版)
rem  用法:
rem    build.bat                编译
rem    build.bat flash          编译并烧录 (默认 COM3)
rem    build.bat flash COM8     编译并烧录 (指定串口)
rem    build.bat monitor        打开串口监视 (Ctrl+] 退出)
rem    build.bat menuconfig     打开菜单配置
rem    build.bat clean          清理
rem    build.bat fullclean      彻底清理
rem =====================================================================

setlocal enabledelayedexpansion

rem ---------- ESP-IDF 安装路径 (换电脑时改这里) ----------
set "IDF_TOOLS_PATH=D:\APP\Espressif"
set "IDF_PATH=D:\APP\Espressif\frameworks\esp-idf-v5.3.1"

rem ---------- 参数解析 ----------
set "ACTION=%~1"
set "PORT=%~2"
if "%ACTION%"=="" set "ACTION=build"
if "%PORT%"=="" set "PORT=COM3"

rem ---------- 检查 ESP-IDF ----------
if not exist "%IDF_PATH%\export.bat" (
    echo [错误] 未找到 ESP-IDF: %IDF_PATH%
    echo 请修改本脚本顶部的 IDF_TOOLS_PATH / IDF_PATH 为实际安装路径
    exit /b 1
)

rem ---------- 激活 ESP-IDF 环境 ----------
echo [1/2] 激活 ESP-IDF 环境...
call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo [错误] ESP-IDF 环境激活失败
    exit /b 1
)

rem ---------- 切换到脚本所在目录并执行 ----------
cd /d "%~dp0"

echo [2/2] 执行 idf.py %ACTION% (端口: %PORT%) ...
if /i "%ACTION%"=="flash" (
    idf.py -p %PORT% flash
) else if /i "%ACTION%"=="monitor" (
    idf.py -p %PORT% monitor
) else (
    idf.py %ACTION%
)

if errorlevel 1 (
    echo.
    echo [失败] idf.py %ACTION% 出错
    exit /b 1
)

echo.
echo [完成] %ACTION% 成功
endlocal
