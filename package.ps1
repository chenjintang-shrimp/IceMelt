<#
.SYNOPSIS
    IceMelt 打包：把一次完整构建的输出拆成两个部署包（命令行 / 图形）。

.DESCRIPTION
    两个包各带一份完整的运行期资产（WinDisk 驱动、kdu、名单、ntfs-3g 工具），因此目标
    机器上只放其中一个就能跑：

        dist/IceMelt-<版本>-cli/      .zip   命令行前端 icemelt.exe + preflight.bat / melt.bat
        dist/IceMelt-GUI-<版本>-gui/  .zip   图形前端 icemelt-gui.exe

    版本默认取 xmake.lua 的 set_version("x.y.z")（统一补上前缀 v）；CI 里传 tag 名。
    任何一项资产缺失都直接失败 —— 宁可不打包，也不出半可用的包。

.PARAMETER BuildDir
    构建输出目录，默认 build/windows/x64/release。

.PARAMETER OutDir
    打包落位目录，默认 dist。

.PARAMETER Version
    版本号（可带或不带 v 前缀）。缺省从 xmake.lua 读。

.EXAMPLE
    pwsh -File package.ps1
    pwsh -File package.ps1 -Version v1.0.0
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build/windows/x64/release' }
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

# 版本：默认读 xmake.lua 的 set_version("1.0.0")；zip 名统一以 v 开头
if (-not $Version) {
    $m = Select-String -Path (Join-Path $root 'xmake.lua') -Pattern '^\s*set_version\("([^"]+)"\)' |
        Select-Object -First 1
    if (-not $m) { throw 'cannot read set_version from xmake.lua (pass -Version instead)' }
    $Version = $m.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^v') { $Version = "v$Version" }

# 两个包共同的运行期资产（相对 exe 目录解析，缺一个就跑不起来）
$runtime = @('WinDisk_x64.sys', 'kdu.exe', 'drv64.dll', 'targets.txt', 'tools')

$packages = @(
    [pscustomobject]@{
        Dir   = "IceMelt-$Version-cli"
        Exe   = 'icemelt.exe'
        Extra = @('preflight.bat', 'melt.bat')
    }
    [pscustomobject]@{
        Dir   = "IceMelt-GUI-$Version-gui"
        Exe   = 'icemelt-gui.exe'
        Extra = @()
    }
)

foreach ($pkg in $packages) {
    foreach ($item in (@($pkg.Exe) + $runtime)) {
        if (-not (Test-Path (Join-Path $BuildDir $item))) {
            throw "$item is missing under $BuildDir -- build first (xmake build)"
        }
    }
    foreach ($item in $pkg.Extra) {
        if (-not (Test-Path (Join-Path $root $item))) {
            throw "$item is missing in the repository root"
        }
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($pkg in $packages) {
    $staged = Join-Path $OutDir $pkg.Dir
    if (Test-Path $staged) { Remove-Item $staged -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $staged | Out-Null

    Copy-Item (Join-Path $BuildDir $pkg.Exe) $staged
    foreach ($item in $pkg.Extra) { Copy-Item (Join-Path $root $item) $staged }
    foreach ($item in $runtime) { Copy-Item (Join-Path $BuildDir $item) $staged -Recurse }

    # zip 里保留一层同名目录：解压到哪都不会把文件撒得到处都是
    $zip = Join-Path $OutDir ($pkg.Dir + '.zip')
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $staged -DestinationPath $zip

    $files = (Get-ChildItem $staged -Recurse -File).Count
    Write-Host ("packaged {0} ({1} files, {2:N1} MB)" -f $zip, $files, ((Get-Item $zip).Length / 1MB))
}
