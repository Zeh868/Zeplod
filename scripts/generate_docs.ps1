# generate_docs.ps1
# 生成 API 文档的 PowerShell 脚本
# 使用方法：.\scripts\generate_docs.ps1

[CmdletBinding()]
param(
    [switch]$Open,          # 生成后自动打开文档
    [switch]$Clean          # 先清理旧文档
)

$ErrorActionPreference = "Stop"

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "生成 API 文档" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "project_layout.ps1")
$Layout = Initialize-ZephyrProjectLayout -ScriptsDir $ScriptDir
$ProjectRoot = $Layout.FrameworkRoot

Set-Location $ProjectRoot

Write-ZephyrProjectBanner -Layout $Layout
Write-Host "文档根目录：$ProjectRoot" -ForegroundColor Gray
Write-Host ""

# 检查 Doxygen 是否安装
Write-Host "检查 Doxygen..." -ForegroundColor Gray
try {
    $doxygenVersion = doxygen --version 2>&1
    Write-Host "  Doxygen 版本：$doxygenVersion" -ForegroundColor Green
} catch {
    Write-Host "  错误：未找到 Doxygen，请先安装" -ForegroundColor Red
    Write-Host ""
    Write-Host "  安装方法：" -ForegroundColor Yellow
    Write-Host "  - 使用 Chocolatey: choco install doxygen.install graphviz" -ForegroundColor Gray
    Write-Host "  - 使用 winget:     winget install doxygen.graphviz" -ForegroundColor Gray
    Write-Host "  - 手动下载：https://www.doxygen.nl/download.html" -ForegroundColor Gray
    Write-Host ""
    exit 1
}

# 检查 Doxyfile 是否存在
if (-not (Test-Path "Doxyfile")) {
    Write-Host "  错误：未找到 Doxyfile" -ForegroundColor Red
    exit 1
}

# 清理旧文档（如果指定了 -Clean）
if ($Clean) {
    Write-Host "清理旧文档..." -ForegroundColor Gray
    if (Test-Path "docs\api\html") {
        Remove-Item -Recurse -Force "docs\api\html"
        Write-Host "  已清理 docs\api\html" -ForegroundColor Gray
    }
    if (Test-Path "docs\api\latex") {
        Remove-Item -Recurse -Force "docs\api\latex"
        Write-Host "  已清理 docs\api\latex" -ForegroundColor Gray
    }
    Write-Host ""
}

# 创建输出目录
Write-Host "创建输出目录..." -ForegroundColor Gray
New-Item -ItemType Directory -Force -Path "docs\api\html" | Out-Null
New-Item -ItemType Directory -Force -Path "docs\api\latex" | Out-Null
Write-Host "  docs\api\html" -ForegroundColor Gray
Write-Host "  docs\api\latex" -ForegroundColor Gray
Write-Host ""

# 生成文档
Write-Host "正在生成文档..." -ForegroundColor Cyan
Write-Host ""

try {
    # 运行 Doxygen
    $output = doxygen Doxyfile 2>&1

    # 显示输出
    if ($output) {
        $output | ForEach-Object {
            if ($_ -match "error|警告|warning") {
                Write-Host "  $_" -ForegroundColor Yellow
            } else {
                Write-Host "  $_" -ForegroundColor Gray
            }
        }
    }

    Write-Host ""
    Write-Host "============================================" -ForegroundColor Green
    Write-Host "文档生成成功！" -ForegroundColor Green
    Write-Host "============================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "输出目录：" -ForegroundColor Cyan
    Write-Host "  HTML: $ProjectRoot\docs\api\html" -ForegroundColor Gray
    Write-Host "  LaTeX: $ProjectRoot\docs\api\latex" -ForegroundColor Gray
    Write-Host ""

    # 自动打开文档（如果指定了 -Open）
    if ($Open) {
        Write-Host "正在打开文档..." -ForegroundColor Gray
        Start-Process "$ProjectRoot\docs\api\html\index.html"
    } else {
        Write-Host "打开文档：" -ForegroundColor Cyan
        Write-Host "  命令：Start-Process 'docs\api\html\index.html'" -ForegroundColor Gray
        Write-Host "  或添加 -Open 参数自动打开" -ForegroundColor Gray
    }
    Write-Host ""

} catch {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Red
    Write-Host "文档生成失败！" -ForegroundColor Red
    Write-Host "============================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "错误信息：$_" -ForegroundColor Red
    Write-Host ""
    exit 1
}
