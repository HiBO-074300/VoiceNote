# ESP32 工程一键备份脚本
# 用法: .\backup.ps1                       备份到工程上一级目录
#       .\backup.ps1 -OutDir D:\备份       指定备份目录
#
# 说明: 只打包源码和配置，自动排除 build/（编译产物，可随时重新生成）、.git/、日志。

param(
    [string]$OutDir = '',
    [switch]$IncludeBuild
)

$ErrorActionPreference = 'Stop'

$src = $PSScriptRoot
$projName = Split-Path $src -Leaf

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Split-Path $src -Parent
}
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$zip = Join-Path $OutDir ("{0}_{1}.zip" -f $projName, $stamp)

$sz = 'D:\APP\7-Zip\7z.exe'

if ((Test-Path $sz) -and -not $IncludeBuild) {
    # 7-Zip：直接从源目录打包并排除目录，无需拷贝
    $args7z = @('a', '-tzip', '-mcu=on', $zip, (Join-Path $src '*'), '-xr!build', '-xr!.git', '-xr!.cache', '-xr!*.log', '-xr!build*')
    & $sz @args7z | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '7-Zip 打包失败' }
}
else {
    # 回退方案：先拷到临时目录剔除 build，再用系统压缩
    $tmp = Join-Path $env:TEMP ("espbak_{0}_{1}" -f $projName, $stamp)
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null

    $exclude = @('.git', '.cache')
    if (-not $IncludeBuild) { $exclude += 'build' }

    Get-ChildItem -Path $src -Force | Where-Object { $exclude -notcontains $_.Name } |
        ForEach-Object { Copy-Item -Path $_.FullName -Destination $tmp -Recurse -Force }

    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip
    Remove-Item $tmp -Recurse -Force
}

$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Write-Host "备份完成: $zip  ($sizeMB MB)" -ForegroundColor Green
