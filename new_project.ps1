# ESP32 新建工程脚本（以当前工程为模板）
# 用法: .\new_project.ps1 -Name my_led
#       .\new_project.ps1 -Name my_led -OutDir D:\DATA\projects
#
# 生成: <OutDir>\<Name>\
#         CMakeLists.txt   项目名已替换为 <Name>
#         main\CMakeLists.txt
#         main\main.c
#         build.ps1  backup.ps1  new_project.ps1  .gitignore  使用教程.md

param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'

$src = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Split-Path $src -Parent
}
$dest = Join-Path $OutDir $Name

if (Test-Path $dest) {
    throw "目标目录已存在: $dest"
}

New-Item -ItemType Directory -Path $dest -Force | Out-Null

# 拷贝源码与脚本，跳过编译产物和 git
Get-ChildItem -Path $src -Force | Where-Object { @('build', '.git') -notcontains $_.Name } |
    ForEach-Object { Copy-Item -Path $_.FullName -Destination $dest -Recurse -Force }

# 顶层 CMakeLists.txt 里替换工程名
$topCmake = Join-Path $dest 'CMakeLists.txt'
if (Test-Path $topCmake) {
    $content = Get-Content $topCmake -Raw
    $content = [regex]::Replace($content, 'project\s*\(\s*[\w\-]+\s*\)', "project($Name)")
    Set-Content -Path $topCmake -Value $content -NoNewline -Encoding UTF8
}

Write-Host "工程已创建: $dest" -ForegroundColor Green
Write-Host "下一步:" -ForegroundColor Yellow
Write-Host "  cd `"$dest`""
Write-Host "  .\build.ps1"
Write-Host "  .\build.ps1 flash -Port COM3"
