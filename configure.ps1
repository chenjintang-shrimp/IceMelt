<#
.SYNOPSIS
    SecMelt 的 configure 步骤：检测构建依赖，报告缺什么、怎么补。

.DESCRIPTION
    在做 `xmake build` 之前跑一次，确认工具链齐了。全部必需项都在时退出码 0，
    否则 1（可直接用于 CI）。

    覆盖的依赖：xmake、Visual Studio（clang-cl + link.exe）、WDK、mingw-w64 工具链、
    生成 ntfs-3g config.h 用的 POSIX shell、KDU submodule。

.PARAMETER Quiet
    只输出缺失项与汇总。

.EXAMPLE
    pwsh -File configure.ps1
    pwsh -File configure.ps1 -Quiet
#>
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }

# MSYS_ROOT 只在找 config.h 生成所需的 shell 时用；MINGW_ROOT 指向 mingw-w64 工具链。
$msysRoot = if ($env:MSYS_ROOT) { $env:MSYS_ROOT } else { 'D:\msys64' }
$mingwRoot = if ($env:MINGW_ROOT) { $env:MINGW_ROOT } else { Join-Path $msysRoot 'mingw64' }
$winKits = if ($env:WindowsSdkDir) { $env:WindowsSdkDir.TrimEnd('\') } else { 'C:\Program Files (x86)\Windows Kits\10' }

$script:results = @()

function Add-Result {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail,
        [string]$Fix,
        [bool]$Required = $true
    )
    $script:results += [pscustomobject]@{
        Name     = $Name
        Ok       = $Ok
        Detail   = $Detail
        Fix      = $Fix
        Required = $Required
    }
}

function Write-Result {
    param([object]$R)
    $tag = if ($R.Ok) { 'ok  ' } else { 'MISS' }
    $color = if ($R.Ok) { 'Green' } else { if ($R.Required) { 'Red' } else { 'Yellow' } }
    if ($Quiet -and $R.Ok) { return }
    Write-Host ("  [{0}] {1,-22} {2}" -f $tag, $R.Name, $R.Detail) -ForegroundColor $color
    if (-not $R.Ok -and $R.Fix) {
        Write-Host ("         -> " + $R.Fix) -ForegroundColor Yellow
    }
}

# --- xmake --------------------------------------------------------------------
$cmd = Get-Command xmake -ErrorAction SilentlyContinue
$ok = [bool]$cmd
Add-Result 'xmake' $ok ($(if ($ok) { (& xmake --version 2>&1 | Select-Object -First 1) } else { 'not found in PATH' })) `
    'install from https://xmake.io, then make sure it is on PATH'

# --- Visual Studio：clang-cl 与 link.exe -------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
}
if ($vsPath) { $vsPath = $vsPath.Trim() }
$ok = [bool]$vsPath
Add-Result 'Visual Studio' $ok ($(if ($ok) { $vsPath } else { 'no installation found by vswhere' })) `
    'install VS with the "Desktop development with C++" workload'

$clangCl = $null
$linkExe = $null
if ($vsPath) {
    $clangCl = Get-ChildItem (Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe') -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    # 可能装了多个 MSVC 工具集，取版本最新的那个（与 xmake 的选择一致）
    $linkExe = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\link.exe') -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$ok = [bool]$clangCl
Add-Result 'clang-cl' $ok ($(if ($ok) { $clangCl } else { 'not found under VC\Tools\Llvm' })) `
    'add the "C++ Clang Compiler for Windows" individual component to VS'
$ok = [bool]$linkExe
Add-Result 'link.exe' $ok ($(if ($ok) { $linkExe } else { 'not found under VC\Tools\MSVC' })) `
    'the MSVC toolset is missing; add the C++ workload'

# --- WDK：内核头 + Win7 目标要用的那个库 --------------------------------------
$kmInclude = Get-ChildItem (Join-Path $winKits 'Include\*\km') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
$ok = [bool]$kmInclude
$detail = if ($ok) { 'km headers: ' + $kmInclude.FullName } else { 'no Include\<ver>\km under ' + $winKits }
Add-Result 'WDK' $ok $detail 'install the Windows Driver Kit matching your VS; set WindowsSdkDir if it is elsewhere'

if ($kmInclude) {
    # kmInclude 是 ...\Include\<ver>\km，版本号在它的父目录名上
    $ver = Split-Path (Split-Path $kmInclude.FullName -Parent) -Leaf
    $blo = Join-Path $winKits ("Lib\$ver\km\x64\BufferOverflowK.lib")
    $ok = Test-Path $blo
    Add-Result 'BufferOverflowK.lib' $ok ($(if ($ok) { "$ver" } else { "missing for WDK $ver" })) `
        'this library is what a Windows 7 target links against; a WDK without it cannot build the driver for Win7'
}

# --- mingw-w64 工具链 ---------------------------------------------------------
$gcc = Join-Path $mingwRoot 'bin\gcc.exe'
$ok = Test-Path $gcc
Add-Result 'mingw-w64 gcc' $ok ($(if ($ok) { $gcc } else { 'not found at ' + $gcc })) `
    'MSYS2: pacman -S mingw-w64-x86_64-gcc; or set MINGW_ROOT to any mingw-w64 toolchain'

if ($ok) {
    $missing = @()
    foreach ($tool in 'gcc.exe', 'g++.exe', 'ar.exe') {
        if (-not (Test-Path (Join-Path $mingwRoot "bin\$tool"))) { $missing += $tool }
    }
    Add-Result 'mingw-w64 binutils' ($missing.Count -eq 0) `
        $(if ($missing.Count -eq 0) { 'gcc / g++ / ar present' } else { 'missing: ' + ($missing -join ', ') }) `
        'reinstall the mingw-w64 gcc package'

    # CRT 变体：mingw64（msvcrt）能在 Win7 直接跑；ucrt64 需要目标机另装 UCRT。
    $probeDir = Join-Path ([IO.Path]::GetTempPath()) ('secmelt-probe-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
        $src = Join-Path $probeDir 'probe.c'
        Set-Content -Path $src -Value 'int main(void){return 0;}' -Encoding ASCII
        $exe = Join-Path $probeDir 'probe.exe'
        & $gcc -o $exe $src 2>$null | Out-Null
        if (Test-Path $exe) {
            $bytes = [IO.File]::ReadAllBytes($exe)
            $text = [Text.Encoding]::ASCII.GetString($bytes)
            $isUcrt = $text.Contains('api-ms-win-crt')
            Add-Result 'mingw CRT flavour' (-not $isUcrt) `
                $(if ($isUcrt) { 'UCRT (links api-ms-win-crt-*.dll)' } else { 'msvcrt (links msvcrt.dll)' }) `
                'prefer the msvcrt variant (MSYS2 mingw64, not ucrt64): UCRT needs an extra runtime on Windows 7' $false
        } else {
            Add-Result 'mingw CRT flavour' $false 'probe did not link' 'the toolchain cannot produce an executable'
        }
    } finally {
        Remove-Item $probeDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- config.h：存在就不需要探测工具链 -----------------------------------------
$configH = Join-Path $root 'third_party\ntfs-3g\build\mingw\config.h'
$ok = Test-Path $configH
Add-Result 'ntfs-3g config.h' $ok `
    $(if ($ok) { 'present' } else { 'not generated yet' }) `
    'generated automatically on first build by genconfig.ps1 (needs gcc + PowerShell)' $false

# --- PowerShell：首次生成 config.h 时会用到（本脚本自己就跑在里面）-------------
# 构建脚本优先用 pwsh，找不到就退回 Windows PowerShell 5.1；genconfig.ps1 两者都能跑。
$pwshPath = Get-Command pwsh -ErrorAction SilentlyContinue
$picked = if ($pwshPath) { $pwshPath.Source } else { "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" }
Add-Result 'PowerShell' $true $picked 'used to probe the toolchain and write ntfs-3g config.h' $false

# --- KDU submodule ------------------------------------------------------------
$kduMarker = Join-Path $root 'third_party\KDU\Source\Hamakaze\kduprov.cpp'
$ok = Test-Path $kduMarker
Add-Result 'KDU submodule' $ok `
    $(if ($ok) { 'sources present' } else { 'third_party\KDU is empty' }) `
    'git submodule update --init --recursive'

# --- 汇总 ---------------------------------------------------------------------
foreach ($r in $script:results) { Write-Result $r }

$failed = @($script:results | Where-Object { -not $_.Ok -and $_.Required })
Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host 'All required dependencies are present.' -ForegroundColor Green
    Write-Host '  xmake f --yes -p windows -a x64 --toolchain=clang-cl   # once' -ForegroundColor Gray
    Write-Host '  xmake build' -ForegroundColor Gray
    exit 0
}

Write-Host ("Missing {0} required dependency/dependencies: {1}" -f $failed.Count, (($failed | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Red
exit 1
