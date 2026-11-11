#!/usr/bin/env pwsh
# =============================================================================
# Zephyr Map 文件分析脚本 (PowerShell)
# =============================================================================
# 用法：.\scripts\analyze_map.ps1 [map 文件路径]
# =============================================================================

param(
    [string]$MapFile,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

# 帮助信息
if ($Help) {
    Write-Host "用法：.\analyze_map.ps1 [选项]"
    Write-Host ""
    Write-Host "选项:"
    Write-Host "  -MapFile PATH    Map 文件路径（默认：release/*.map 中最新的文件）"
    Write-Host "  -Help            显示帮助"
    exit 0
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "project_layout.ps1")
$Layout = Initialize-ZephyrProjectLayout -ScriptsDir $ScriptDir
$ProjectRoot = $Layout.WorkRoot

Write-ZephyrProjectBanner -Layout $Layout
Write-Host ""

if (-not $MapFile) {
    $ReleaseDir = Join-Path $ProjectRoot "release"
    if (Test-Path $ReleaseDir) {
        $MapFile = Get-ChildItem -Path $ReleaseDir -Filter "*.map" |
                   Sort-Object LastWriteTime -Descending |
                   Select-Object -First 1 -ExpandProperty FullName
    }
}

if (-not $MapFile -or -not (Test-Path $MapFile)) {
    Write-Host "错误：找不到 Map 文件！" -ForegroundColor Red
    Write-Host "用法：.\analyze_map.ps1 -MapFile <path>"
    exit 1
}

Write-Host "============================================"
Write-Host "Zephyr Map 文件分析工具"
Write-Host "============================================"
Write-Host "分析文件：$MapFile"
Write-Host ""

# 读取文件内容
$Content = Get-Content $MapFile -Raw

# =============================================================================
# 解析内存配置
# =============================================================================
Write-Host "============================================"
Write-Host "1. 内存配置"
Write-Host "============================================"

$MemoryConfig = [regex]::Matches($Content,
    'Name\s+([A-Za-z0-9]+)\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)')

$MemoryTable = @()
foreach ($Match in $MemoryConfig) {
    $Name = $Match.Groups[1].Value
    $Origin = [Convert]::ToInt64($Match.Groups[2].Value, 16)
    $Length = [Convert]::ToInt64($Match.Groups[3].Value, 16)

    $MemoryTable += [PSCustomObject]@{
        Name   = $Name
        Origin = "0x{0:X8}" -f $Origin
        Length = "{0:N0} ({1:N2} KB)" -f $Length, ($Length / 1KB)
    }
}

$MemoryTable | Format-Table -AutoSize

# =============================================================================
# 解析 BSS 段
# =============================================================================
Write-Host "============================================"
Write-Host "2. BSS 段 (未初始化数据) 分布"
Write-Host "============================================"

$BssMatches = [regex]::Matches($Content, '\.bss\.([^\s]+)\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+([^\s]+)')

$BssData = @()
foreach ($Match in $BssMatches) {
    $Name = $Match.Groups[1].Value
    $Address = [Convert]::ToInt64($Match.Groups[2].Value, 16)
    $Size = [Convert]::ToInt64($Match.Groups[3].Value, 16)
    $Module = $Match.Groups[4].Value

    if ($Size -gt 0) {
        $BssData += [PSCustomObject]@{
            Variable = $Name
            Size     = $Size
            SizeKB   = [math]::Round($Size / 1KB, 2)
            Module   = (Split-Path -Leaf $Module) -replace '\.obj$', ''
        }
    }
}

# 按大小排序并显示前 20 名
$TopBss = $BssData | Sort-Object Size -Descending | Select-Object -First 20

$TopBss | Format-Table -Property Variable, @{Label="Size (Bytes)";Expression={$_.Size}}, @{Label="Size (KB)";Expression={$_.SizeKB}}, Module -AutoSize

# BSS 总计
$TotalBss = ($BssData | Measure-Object Size -Sum).Sum
Write-Host ("BSS 段总计：{0:N0} 字节 ({1:N2} KB)" -f $TotalBss, ($TotalBss / 1KB))
Write-Host ""

# =============================================================================
# 解析 NOINIT 段
# =============================================================================
Write-Host "============================================"
Write-Host "3. NOINIT 段 (不初始化数据) 分布"
Write-Host "============================================"

$NoinitMatches = [regex]::Matches($Content, '\.noinit\.([^\s]+)\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)')

$NoinitData = @()
foreach ($Match in $NoinitMatches) {
    $Name = $Match.Groups[1].Value -replace '^WEST_TOPDIR/', ''
    $Address = [Convert]::ToInt64($Match.Groups[2].Value, 16)
    $Size = [Convert]::ToInt64($Match.Groups[3].Value, 16)

    if ($Size -gt 0) {
        $NoinitData += [PSCustomObject]@{
            Section = $Name
            Size    = $Size
            SizeKB  = [math]::Round($Size / 1KB, 2)
        }
    }
}

$NoinitData | Sort-Object Size -Descending | Format-Table -AutoSize

$TotalNoinit = ($NoinitData | Measure-Object Size -Sum).Sum
Write-Host ("NOINIT 段总计：{0:N0} 字节 ({1:N2} KB)" -f $TotalNoinit, ($TotalNoinit / 1KB))
Write-Host ""

# =============================================================================
# 解析代码段 (按模块统计)
# =============================================================================
Write-Host "============================================"
Write-Host "4. 代码段 (.text) 按模块分布"
Write-Host "============================================"

# 按 .obj 文件统计代码大小
$TextByModule = @{}
$TextMatches = [regex]::Matches($Content, '\.text[^\s]*\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+([^\s]+\.obj)')

foreach ($Match in $TextMatches) {
    $Size = [Convert]::ToInt64($Match.Groups[2].Value, 16)
    $Module = $Match.Groups[3].Value

    if ($TextByModule.ContainsKey($Module)) {
        $TextByModule[$Module] += $Size
    } else {
        $TextByModule[$Module] = $Size
    }
}

$ModuleTable = @()
foreach ($Key in $TextByModule.Keys) {
    $ModuleTable += [PSCustomObject]@{
        Module = (Split-Path -Leaf $Key) -replace '\.obj$', ''
        Size   = $TextByModule[$Key]
        SizeKB = [math]::Round($TextByModule[$Key] / 1KB, 2)
    }
}

$ModuleTable | Sort-Object Size -Descending | Format-Table -AutoSize

$TotalText = ($ModuleTable | Measure-Object Size -Sum).Sum
Write-Host ("代码段总计：{0:N0} 字节 ({1:N2} KB)" -f $TotalText, ($TotalText / 1KB))
Write-Host ""

# =============================================================================
# 解析只读数据段
# =============================================================================
Write-Host "============================================"
Write-Host "5. 只读数据段 (.rodata) 分布"
Write-Host "============================================"

$RodataMatches = [regex]::Matches($Content, '\.rodata[^\s]*\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+([^\s]+)')

$RodataData = @{}
foreach ($Match in $RodataMatches) {
    $Size = [Convert]::ToInt64($Match.Groups[2].Value, 16)
    $Module = $Match.Groups[3].Value

    if ($RodataData.ContainsKey($Module)) {
        $RodataData[$Module] += $Size
    } else {
        $RodataData[$Module] = $Size
    }
}

$RodataTable = @()
foreach ($Key in $RodataData.Keys) {
    if ($RodataData[$Key] -gt 0) {
        $RodataTable += [PSCustomObject]@{
            Module = (Split-Path -Leaf $Key) -replace '\.obj$', ''
            Size   = $RodataData[$Key]
            SizeKB = [math]::Round($RodataData[$Key] / 1KB, 2)
        }
    }
}

$RodataTable | Sort-Object Size -Descending | Select-Object -First 15 | Format-Table -AutoSize

$TotalRodata = ($RodataTable | Measure-Object Size -Sum).Sum
Write-Host ("只读数据段总计：{0:N0} 字节 ({1:N2} KB)" -f $TotalRodata, ($TotalRodata / 1KB))
Write-Host ""

# =============================================================================
# 内存使用总结
# =============================================================================
Write-Host "============================================"
Write-Host "6. 内存使用总结"
Write-Host "============================================"

$RamTotal = 192 * 1KB  # STM32L4 典型值
$RamUsed = $TotalBss + $TotalNoinit
$RamPercent = [math]::Round(($RamUsed / $RamTotal) * 100, 2)

Write-Host "RAM 使用情况:"
Write-Host ("  BSS 段：     {0,12:N0} 字节 ({1,8:N2} KB)" -f $TotalBss, ($TotalBss / 1KB))
Write-Host ("  NOINIT 段：  {0,12:N0} 字节 ({1,8:N2} KB)" -f $TotalNoinit, ($TotalNoinit / 1KB))
Write-Host "  ─────────────────────────────"
Write-Host ("  已使用：     {0,12:N0} 字节 ({1,8:N2} KB) ({2}%)" -f $RamUsed, ($RamUsed / 1KB), $RamPercent)
Write-Host ("  总容量：     {0,12:N0} 字节 ({1,8:N2} KB)" -f $RamTotal, ($RamTotal / 1KB))
Write-Host ("  剩余可用：   {0,12:N0} 字节 ({1,8:N2} KB)" -f ($RamTotal - $RamUsed), (($RamTotal - $RamUsed) / 1KB))
Write-Host ""

$FlashTotal = 2 * 1MB  # 典型 Flash 大小
$FlashUsed = $TotalText + $TotalRodata
$FlashPercent = [math]::Round(($FlashUsed / $FlashTotal) * 100, 2)

Write-Host "Flash 使用情况:"
Write-Host ("  代码段：     {0,12:N0} 字节 ({1,8:N2} KB)" -f $TotalText, ($TotalText / 1KB))
Write-Host ("  只读数据：   {0,12:N0} 字节 ({1,8:N2} KB)" -f $TotalRodata, ($TotalRodata / 1KB))
Write-Host "  ─────────────────────────────"
Write-Host ("  已使用：     {0,12:N0} 字节 ({1,8:N2} KB) ({2}%)" -f $FlashUsed, ($FlashUsed / 1KB), $FlashPercent)
Write-Host ("  总容量：     {0,12:N0} 字节 ({1,8:N2} KB)" -f $FlashTotal, ($FlashTotal / 1KB))
Write-Host ("  剩余可用：   {0,12:N0} 字节 ({1,8:N2} KB)" -f ($FlashTotal - $FlashUsed), (($FlashTotal - $FlashUsed) / 1KB))
Write-Host ""

# =============================================================================
# 输出到文件
# =============================================================================
$OutputFile = Join-Path $ProjectRoot "release\memory_analysis.txt"
$StringBuilder = New-Object System.Text.StringBuilder

[string]$null = $StringBuilder.AppendLine("============================================")
[string]$null = $StringBuilder.AppendLine("Zephyr Map 文件分析报告")
[string]$null = $StringBuilder.AppendLine("============================================")
[string]$null = $StringBuilder.AppendLine("分析文件：$MapFile")
[string]$null = $StringBuilder.AppendLine("生成时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
[string]$null = $StringBuilder.AppendLine("")

[string]$null = $StringBuilder.AppendLine("=== BSS 段 Top 20 ===")
[string]$null = $StringBuilder.AppendLine(($TopBss | Format-Table -AutoSize | Out-String))
[string]$null = $StringBuilder.AppendLine("")

[string]$null = $StringBuilder.AppendLine("=== 代码段按模块 ===")
[string]$null = $StringBuilder.AppendLine(($ModuleTable | Sort-Object Size -Descending | Format-Table -AutoSize | Out-String))
[string]$null = $StringBuilder.AppendLine("")

[string]$null = $StringBuilder.AppendLine("=== 内存使用总结 ===")
[string]$null = $StringBuilder.AppendLine(("RAM 使用：{0:N0} 字节 ({1:N2} KB) / {2:N0} 字节 ({3:N2} KB) = {4}%" -f
    $RamUsed, ($RamUsed / 1KB), $RamTotal, ($RamTotal / 1KB), $RamPercent))
[string]$null = $StringBuilder.AppendLine(("Flash 使用：{0:N0} 字节 ({1:N2} KB) / {2:N0} 字节 ({3:N2} KB) = {4}%" -f
    $FlashUsed, ($FlashUsed / 1KB), $FlashTotal, ($FlashTotal / 1KB), $FlashPercent))

$StringBuilder.ToString() | Out-File -FilePath $OutputFile -Encoding UTF8
Write-Host "详细报告已保存到：$OutputFile" -ForegroundColor Green
Write-Host ""
