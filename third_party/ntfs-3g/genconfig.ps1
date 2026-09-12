<#
.SYNOPSIS
    为 mingw-w64 目标生成 ntfs-3g 的 config.h，不需要 POSIX shell。

.DESCRIPTION
    autoconf 的 configure 本身就是一份 /bin/sh 脚本（19000 余行），没有 shell 跑不起来，
    所以在纯 Windows 环境下只能把它的探测复刻一遍。本脚本用 gcc 做这些探测，并尽量
    照抄 autoconf 的判定方式：

      * 头文件     编译一个只 include 的翻译单元（-fsyntax-only）
      * 函数       与 autoconf 完全相同的程序：先把名字定义成无害变体、include <limits.h>、
                   再 #undef、然后 char NAME(); 并真实链接
      * 成员/类型  构造真实用法后编译
      * 其余       由 autoconf 宏或我们的固定 configure 选项决定，逐条在下面写明依据

    探测结果与 autoconf 的产出逐字节一致（可 diff 复核）。

.PARAMETER Gcc
    mingw-w64 gcc 的路径。

.PARAMETER Out
    输出的 config.h 路径。

.PARAMETER Template
    config.h.in 的路径（默认取脚本同目录下的 config.h.in）。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Gcc,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Template
)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = (Get-Location).Path }
if (-not $Template) { $Template = Join-Path $scriptDir 'config.h.in' }

if (-not (Test-Path $Gcc)) { throw "gcc not found: $Gcc" }
if (-not (Test-Path $Template)) { throw "template not found: $Template" }

$probe = Join-Path ([IO.Path]::GetTempPath()) ('ntfs3g-cfg-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $probe -Force | Out-Null

$probed = @{}          # 名字 -> $true/$false（探测所得）
$forced = @{}          # 名字 -> @{ on = $bool; why = '...' }（显式覆盖，优先于探测）
$confdefs = [System.Collections.Generic.List[string]]::new()
$probeSeq = 0

# 调用 gcc 并只取退出码。
# 两点注意（都是 Windows PowerShell 5.1 的坑，pwsh 下同样成立）：
#   * 不能写 `2>$null`：5.1 里原生命令往 stderr 写东西、又开着 ErrorActionPreference=Stop 时
#     会抛 NativeCommandError。这里用 2>&1 合并后丢弃，再把 EAP 临时降到 Continue。
#   * 判据是退出码，不看产物是否存在 —— 同一路径的旧产物被杀软锁住时删不掉，
#     会让"文件存在"的判据误报成功。
function Invoke-Gcc {
    param([string[]]$ArgList, [switch]$Capture)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Gcc @ArgList 2>&1
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($Capture) { return ($out | Out-String) }
    return $LASTEXITCODE
}

function Set-Probed { param([string]$Name, [bool]$Value) $script:probed[$Name] = $Value }
function Force { param([string]$Name, [bool]$On, [string]$Why) $script:forced[$Name] = @{ on = $On; why = $Why } }

# 编译/链接一个翻译单元。confdefs.h 按 autoconf 的做法前置（累积已确定的宏）。
# 判定一律看 gcc 的退出码 —— 不要看产物是否存在：同一路径的旧产物被占用（杀软扫描新
# 生成的 exe）时删不掉，会让下一次探测误判为成功。
function Test-CSource {
    param([string]$Source, [switch]$Link)
    $script:probeSeq++
    $c = Join-Path $probe "p$($script:probeSeq).c"
    $o = Join-Path $probe "p$($script:probeSeq).exe"
    $head = ($script:confdefs -join "`n")
    Set-Content -Path $c -Value ($head + "`n" + $Source) -Encoding ASCII
    if ($Link) {
        $code = Invoke-Gcc -ArgList @('-o', $o, $c)
    } else {
        $code = Invoke-Gcc -ArgList @('-fsyntax-only', $c)
    }
    return ($code -eq 0)
}

function Add-Confdef { param([string]$Line) $script:confdefs.Add($Line) }

# --- AC_CHECK_HEADERS ---------------------------------------------------------
# 名字 -> 头文件路径，顺序与内容照抄 configure.ac
$headers = [ordered]@{
    'HAVE_CTYPE_H'          = 'ctype.h'
    'HAVE_FCNTL_H'          = 'fcntl.h'
    'HAVE_LIBGEN_H'         = 'libgen.h'
    'HAVE_LIBINTL_H'        = 'libintl.h'
    'HAVE_LIMITS_H'         = 'limits.h'
    'HAVE_LOCALE_H'         = 'locale.h'
    'HAVE_MNTENT_H'         = 'mntent.h'
    'HAVE_STDDEF_H'         = 'stddef.h'
    'HAVE_STDINT_H'         = 'stdint.h'
    'HAVE_STDLIB_H'         = 'stdlib.h'
    'HAVE_STDIO_H'          = 'stdio.h'
    'HAVE_STDARG_H'         = 'stdarg.h'
    'HAVE_STRING_H'         = 'string.h'
    'HAVE_STRINGS_H'        = 'strings.h'
    'HAVE_ERRNO_H'          = 'errno.h'
    'HAVE_TIME_H'           = 'time.h'
    'HAVE_UNISTD_H'         = 'unistd.h'
    'HAVE_UTIME_H'          = 'utime.h'
    'HAVE_WCHAR_H'          = 'wchar.h'
    'HAVE_GETOPT_H'         = 'getopt.h'
    'HAVE_FEATURES_H'       = 'features.h'
    'HAVE_REGEX_H'          = 'regex.h'
    'HAVE_ENDIAN_H'         = 'endian.h'
    'HAVE_BYTESWAP_H'       = 'byteswap.h'
    'HAVE_SYS_BYTEORDER_H'  = 'sys/byteorder.h'
    'HAVE_SYS_DISK_H'       = 'sys/disk.h'
    'HAVE_SYS_ENDIAN_H'     = 'sys/endian.h'
    'HAVE_SYS_PARAM_H'      = 'sys/param.h'
    'HAVE_SYS_IOCTL_H'      = 'sys/ioctl.h'
    'HAVE_SYS_MOUNT_H'      = 'sys/mount.h'
    'HAVE_SYS_STAT_H'       = 'sys/stat.h'
    'HAVE_SYS_TYPES_H'      = 'sys/types.h'
    'HAVE_SYS_VFS_H'        = 'sys/vfs.h'
    'HAVE_SYS_STATVFS_H'    = 'sys/statvfs.h'
    'HAVE_LINUX_MAJOR_H'    = 'linux/major.h'
    'HAVE_LINUX_FD_H'       = 'linux/fd.h'
    'HAVE_LINUX_FS_H'       = 'linux/fs.h'
    'HAVE_INTTYPES_H'       = 'inttypes.h'
    'HAVE_LINUX_HDREG_H'    = 'linux/hdreg.h'
    'HAVE_MACHINE_ENDIAN_H' = 'machine/endian.h'
    'HAVE_WINDOWS_H'        = 'windows.h'
    'HAVE_SYSLOG_H'         = 'syslog.h'
    'HAVE_PWD_H'            = 'pwd.h'
    'HAVE_MALLOC_H'         = 'malloc.h'
    'HAVE_DLFCN_H'          = 'dlfcn.h'
    'HAVE_MINIX_CONFIG_H'   = 'minix/config.h'
}
foreach ($kv in $headers.GetEnumerator()) {
    $ok = Test-CSource "#include <$($kv.Value)>`nint main(void){return 0;}`n"
    Set-Probed $kv.Key $ok
    if ($ok) { Add-Confdef "#define $($kv.Key) 1" }
}
# libintl.h 在 mingw64 里存在，但我们**故意**不用它：一旦定义了 HAVE_LIBINTL_H，
# utils.c 就会 include <libintl.h>，而该头会把 printf/setlocale/snprintf 重定向到
# libintl_*，凭空多出一个 libintl DLL 依赖（这个工程的目标是产物不带额外运行库 DLL）。
Force 'HAVE_LIBINTL_H' $false '--disable-NLS：避免 libintl DLL 依赖'

# --- AC_CHECK_TYPE(_Bool) + AC_HEADER_STDBOOL ---------------------------------
$boolOk = Test-CSource "#include <stddef.h>`nint main (void) { if (sizeof (_Bool)) return 0; else return 1; }`n"
Set-Probed 'HAVE__BOOL' $boolOk
if ($boolOk) { Add-Confdef '#define HAVE__BOOL 1' }

# AC_HEADER_STDBOOL：与 configure 里那段测试程序一致。mingw 有 stdbool.h，但 gcc 默认
# 语言模式下它通不过该测试，于是 HAVE_STDBOOL_H 不定、由 HAVE__BOOL 兜底。
$stdboolTest = @"
#include <stdbool.h>
#ifndef __bool_true_false_are_defined
# error "__bool_true_false_are_defined is not defined"
#endif
char a[__bool_true_false_are_defined == 1 ? 1 : -1];
#if !true
# error "'true' is not true"
#endif
#if true != 1
# error "'true' is not equal to 1"
#endif
char b[true == 1 ? 1 : -1];
char c[true];
#if false
# error "'false' is not false"
#endif
#if false != 0
# error "'false' is not equal to 0"
#endif
char d[false == 0 ? 1 : -1];
enum { e = false, f = true, g = false * true, h = true * 256 };
char i[(bool) 0.5 == true ? 1 : -1];
char j[(bool) 0.0 == false ? 1 : -1];
char k[sizeof (bool) > 0 ? 1 : -1];
struct sb { bool s: 1; bool t; } s;
char l[sizeof s.t > 0 ? 1 : -1];
bool m[h];
char n[sizeof m == h * sizeof m[0] ? 1 : -1];
char o[-1 - (bool) 0 < 0 ? 1 : -1];
bool p = true;
bool *pp = &p;
#ifndef bool
# error "bool is not defined"
#endif
#ifndef false
# error "false is not defined"
#endif
#ifndef true
# error "true is not defined"
#endif
#ifdef HAVE__BOOL
struct sB { _Bool s: 1; _Bool t; } t;
char q[(_Bool) 0.5 == true ? 1 : -1];
char r[(_Bool) 0.0 == false ? 1 : -1];
char u[sizeof (_Bool) > 0 ? 1 : -1];
char v[sizeof t.t > 0 ? 1 : -1];
_Bool w[h];
char x[sizeof m == h * sizeof m[0] ? 1 : -1];
char y[-1 - (_Bool) 0 < 0 ? 1 : -1];
_Bool z = true;
_Bool *pz = &p;
#endif
int main (void)
{
  bool ps = &s;
  *pp |= p;
  *pp |= ! p;
#ifdef HAVE__BOOL
  _Bool pt = &t;
  *pz |= z;
  *pz |= ! z;
#endif
  return (!a + !b + !c + !d + !e + !f + !g + !h + !i + !j + !k
          + !l + !m + !n + !o + !p + !pp + !ps
#ifdef HAVE__BOOL
          + !q + !r + !u + !v + !w + !x + !y + !z + !pt
#endif
         );
}
"@
$stdboolOk = Test-CSource $stdboolTest
Set-Probed 'HAVE_STDBOOL_H' $stdboolOk
if ($stdboolOk) { Add-Confdef '#define HAVE_STDBOOL_H 1' }

# --- AC_C_BIGENDIAN ----------------------------------------------------------
$empty = Join-Path $probe 'empty.c'
Set-Content -Path $empty -Value '' -Encoding ASCII
$endian = Invoke-Gcc -ArgList @('-dM', '-E', '-x', 'c', $empty) -Capture
$little = ($endian -match '__BYTE_ORDER__\s+__ORDER_LITTLE_ENDIAN__')
Set-Probed 'WORDS_LITTLEENDIAN' $little
if ($little) { Add-Confdef '#define WORDS_LITTLEENDIAN 1' }

# --- AC_CHECK_MEMBERS / AC_STRUCT_ST_BLOCKS ----------------------------------
$memberTests = [ordered]@{
    'HAVE_STRUCT_STAT_ST_RDEV'      = 'st_rdev'
    'HAVE_STRUCT_STAT_ST_ATIM'      = 'st_atim'
    'HAVE_STRUCT_STAT_ST_ATIMESPEC' = 'st_atimespec'
    'HAVE_STRUCT_STAT_ST_ATIMENSEC' = 'st_atimensec'
    'HAVE_STRUCT_STAT_ST_BLOCKS'    = 'st_blocks'
}
foreach ($kv in $memberTests.GetEnumerator()) {
    $src = @"
#include <sys/types.h>
#include <sys/stat.h>
int main (void) { struct stat s; (void) sizeof (s.$($kv.Value)); return 0; }
"@
    $ok = Test-CSource $src
    Set-Probed $kv.Key $ok
    if ($ok) { Add-Confdef "#define $($kv.Key) 1" }
}
# AC_STRUCT_ST_BLOCKS 顺带决定 HAVE_ST_BLOCKS
$stBlocks = $probed['HAVE_STRUCT_STAT_ST_BLOCKS']
Set-Probed 'HAVE_ST_BLOCKS' $stBlocks
if ($stBlocks) { Add-Confdef '#define HAVE_ST_BLOCKS 1' }

# --- AC_CHECK_FUNCS（用 autoconf 的原样程序）--------------------------------
# 程序形态与 configure 里的 ac_fn_c_check_func 一致：先把名字定义成无害变体，
# include <limits.h>，再 #undef，然后声明 char NAME() 并真实链接。
function Test-Func {
    param([string]$Name)
    $src = @"
#define $Name innocuous_$Name
#include <limits.h>
#undef $Name
#ifdef __cplusplus
extern "C"
#endif
char $Name ();
#if defined __stub_$Name || defined __stub___$Name
choke me
#endif
int main (void) { return $Name (); }
"@
    return (Test-CSource $src -Link)
}

$funcs = @(
    'atexit', 'basename', 'daemon', 'dup2', 'fdatasync', 'ffs', 'getopt_long', 'hasmntopt',
    'mbsinit', 'memmove', 'memset', 'realpath', 'regcomp', 'setlocale', 'setxattr',
    'strcasecmp', 'strchr', 'strdup', 'strerror', 'strnlen', 'strsep', 'strtol', 'strtoul',
    'sysconf', 'utime', 'utimensat', 'gettimeofday', 'clock_gettime', 'fork', 'memcpy',
    'random', 'snprintf'
)
foreach ($fn in $funcs) {
    $macro = 'HAVE_' + $fn.ToUpperInvariant()
    $ok = Test-Func $fn
    Set-Probed $macro $ok
    if ($ok) { Add-Confdef "#define $macro 1" }
}

# AC_FUNC_GETMNTENT：getmntent 需要 <mntent.h>（libc/libgen/libmount）
$getmntent = $probed['HAVE_MNTENT_H'] -and (Test-Func 'getmntent')
Set-Probed 'HAVE_GETMNTENT' $getmntent
if ($getmntent) { Add-Confdef '#define HAVE_GETMNTENT 1' }

# AC_FUNC_MBRTOWC
$mbrtowc = Test-Func 'mbrtowc'
Set-Probed 'HAVE_MBRTOWC' $mbrtowc
if ($mbrtowc) { Add-Confdef '#define HAVE_MBRTOWC 1' }

# AC_FUNC_STRFTIME
$strftimeSrc = @"
#include <time.h>
int main (void) { char buf[16]; struct tm t; strftime (buf, sizeof buf, "", &t); return 0; }
"@
$strftime = Test-CSource $strftimeSrc -Link
Set-Probed 'HAVE_STRFTIME' $strftime
if ($strftime) { Add-Confdef '#define HAVE_STRFTIME 1' }

# AC_FUNC_UTIME_NULL：utime(path, NULL) 可用
$utimeNullSrc = @"
#include <sys/types.h>
#include <utime.h>
int main (void) { return utime ("", 0); }
"@
$utimeNull = Test-CSource $utimeNullSrc -Link
Set-Probed 'HAVE_UTIME_NULL' $utimeNull
if ($utimeNull) { Add-Confdef '#define HAVE_UTIME_NULL 1' }

# AC_FUNC_STAT：stat("") 误报成功才算有 bug
$statBugSrc = @"
#include <sys/types.h>
#include <sys/stat.h>
int main (void) { struct stat s; return stat ("", &s) == 0; }
"@
$statBug = -not (Test-CSource $statBugSrc -Link)
Set-Probed 'HAVE_STAT_EMPTY_STRING_BUG' $statBug
if ($statBug) { Add-Confdef '#define HAVE_STAT_EMPTY_STRING_BUG 1' }

# AC_FUNC_VPRINTF：vprintf 存在则定义 HAVE_VPRINTF，否则看 _doprnt
$vprintf = Test-Func 'vprintf'
Set-Probed 'HAVE_VPRINTF' $vprintf
if ($vprintf) { Add-Confdef '#define HAVE_VPRINTF 1' }
$doprnt = (-not $vprintf) -and (Test-Func '_doprnt')
Set-Probed 'HAVE_DOPRNT' $doprnt
if ($doprnt) { Add-Confdef '#define HAVE_DOPRNT 1' }

# AC_HEADER_MAJOR
$mkdev = Test-CSource "#include <sys/mkdev.h>`nint main(void){return 0;}`n"
$sysmacros = Test-CSource "#include <sys/sysmacros.h>`nint main(void){return 0;}`n"
Set-Probed 'MAJOR_IN_MKDEV' $mkdev
Set-Probed 'MAJOR_IN_SYSMACROS' $sysmacros
if ($mkdev) { Add-Confdef '#define MAJOR_IN_MKDEV 1' }
if ($sysmacros) { Add-Confdef '#define MAJOR_IN_SYSMACROS 1' }

# --- 由 autoconf 宏或我们的固定选项决定的部分 --------------------------------
# 这些不是"本机探测"，而是固定语义 + 我们对 configure 的固定选择，逐条写明依据。
$fixedOn = [ordered]@{
    # AC_INIT([ntfs-3g],[2022.10.3],[ntfs-3g-devel@lists.sf.net])
    'PACKAGE_NAME'      = 'ntfs-3g'
    'PACKAGE_TARNAME'   = 'ntfs-3g'
    'PACKAGE_VERSION'   = '2022.10.3'
    'PACKAGE_STRING'    = 'ntfs-3g 2022.10.3'
    'PACKAGE_BUGREPORT' = 'ntfs-3g-devel@lists.sf.net'
    'PACKAGE_URL'       = ''
    'PACKAGE'           = 'ntfs-3g'
    'VERSION'           = '2022.10.3'
    # LT_INIT
    'LT_OBJDIR'         = '.libs/'
    # AC_HEADER_STDC
    'STDC_HEADERS'      = 1
    # AC_SYS_LARGEFILE：mingw 的 off_t 是 32 位
    '_FILE_OFFSET_BITS' = 64
    # 目标三元组命中 *-mingw32*（configure.ac 的 Windows 分支）
    'WINDOWS'           = 1
}
$fixedOff = @(
    'AC_APPLE_UNIVERSAL_BUILD',     # 非 Apple 通用二进制
    'DISABLE_PLUGINS',              # 反向：见下，这里先占位（实际是 ON）
    'ENABLE_CRYPTO',                # 未 --enable-crypto
    'ENABLE_DEBUG',                 # 未 --enable-debug
    'ENABLE_HD',                    # 没有 hd.h / hd_list
    'ENABLE_NFCONV',                # 仅 darwin
    'ENABLE_UUID',                  # 未启用 uuid
    'FUSE_INTERNAL',                # --disable-ntfs-3g
    'IGNORE_MTAB',                  # enable_mtab 默认 yes
    'LSTAT_FOLLOWS_SLASHED_SYMLINK',
    'NO_NTFS_DEVICE_DEFAULT_IO_OPS',# enable_device_default_io_ops 默认 yes
    'POSIXACLS',                    # --enable-posix-acls 默认 no
    'XATTR_MAPPINGS',               # --enable-xattr-mappings 默认 no
    '_LARGE_FILES',
    '_REENTRANT',                   # 未启用 pthread
    'HAVE_LIBC',                    # --disable-plugins 跳过了 AC_CHECK_LIB(c, dlopen)
    'WORDS_BIGENDIAN',              # AC_C_BIGENDIAN：本目标是小端
    'const', 'inline', 'off_t', 'size_t'  # 编译器原生支持，无需定义
)
Force 'DISABLE_PLUGINS' $true '--disable-plugins：插件要 dlopen，mingw 没有'
foreach ($m in $fixedOff) {
    if ($m -ne 'DISABLE_PLUGINS') { Force $m $false '固定选项/平台语义' }
}
foreach ($kv in $fixedOn.GetEnumerator()) { Force $kv.Key $true 'AC_INIT / autoconf 宏 / 固定选项' }
$forcedValues = @{}
foreach ($kv in $fixedOn.GetEnumerator()) { $forcedValues[$kv.Key] = $kv.Value }
$forcedValues['DISABLE_PLUGINS'] = 1

# AC_USE_SYSTEM_EXTENSIONS：autoconf 会为"定义也无害"的那批宏写 # define，其余保持未定义。
# 这批是 autoconf 宏的内部名单，与平台无关；值取自同一 autoconf 版本的产出。
$systemExtensionsOn = @(
    '_ALL_SOURCE', '_DARWIN_C_SOURCE', '__EXTENSIONS__', '_GNU_SOURCE',
    '_HPUX_ALT_XOPEN_SOCKET_API', '_NETBSD_SOURCE', '_OPENBSD_SOURCE',
    '_POSIX_PTHREAD_SEMANTICS', '__STDC_WANT_IEC_60559_ATTRIBS_EXT__',
    '__STDC_WANT_IEC_60559_BFP_EXT__', '__STDC_WANT_IEC_60559_DFP_EXT__',
    '__STDC_WANT_IEC_60559_FUNCS_EXT__', '__STDC_WANT_IEC_60559_TYPES_EXT__',
    '__STDC_WANT_LIB_EXT2__', '__STDC_WANT_MATH_SPEC_FUNCS__', '_TANDEM_SOURCE'
)
$systemExtensionsOff = @('_MINIX', '_POSIX_SOURCE', '_POSIX_1_SOURCE', '_XOPEN_SOURCE')
foreach ($m in $systemExtensionsOn) { Force $m $true 'AC_USE_SYSTEM_EXTENSIONS：定义无害' }
foreach ($m in $systemExtensionsOff) { Force $m $false 'AC_USE_SYSTEM_EXTENSIONS：保持未定义' }

# --- 解析：显式覆盖优先于探测，并报告两者冲突 ---------------------------------
function Resolve-Macro {
    param([string]$Name)
    if ($script:forced.ContainsKey($Name)) {
        $f = $script:forced[$Name]
        if ($script:probed.ContainsKey($Name) -and $script:probed[$Name] -ne $f.on) {
            Write-Warning ("$Name : 探测=$($script:probed[$Name]) 被显式覆盖为 $($f.on)（$($f.why)）")
        }
        return $f.on
    }
    if ($script:probed.ContainsKey($Name)) { return $script:probed[$Name] }
    return $null
}

$templateText = Get-Content -Path $Template -Raw
$unresolved = [System.Collections.Generic.List[string]]::new()
$definedCount = 0

# autoconf 的 config.status 会把 `#<sep>undef NAME` 换成 `#<sep>define NAME 1`，
# 或未定义时换成 `/* #<sep>undef NAME */` —— 注意 <sep>（# 与 undef 之间的空白）原样保留：
# 普通宏是 "#undef"（无空格），AC_USE_SYSTEM_EXTENSIONS 区块是 "# undef"，
# AC_C_BIGENDIAN 那段是 "#  undef"（两个空格）。三种都要还原成一样的样子。
$outText = [regex]::Replace($templateText, '(?m)^#([ \t]*)undef[ \t]+([A-Za-z0-9_]+)[ \t]*$', {
        param($m)
        $sep = $m.Groups[1].Value
        $name = $m.Groups[2].Value
        $undefLine = "/* #${sep}undef $name */"

        $val = Resolve-Macro $name
        if ($val -eq $true) {
            $script:definedCount++
            if ($script:forcedValues.ContainsKey($name)) {
                $v = $script:forcedValues[$name]
                if ($v -is [string]) {
                    if ($v -eq '') { return "#${sep}define $name `"`"" }
                    return "#${sep}define $name `"$v`""
                }
                return "#${sep}define $name $v"
            }
            return "#${sep}define $name 1"
        }
        elseif ($val -eq $false) {
            return $undefLine
        }
        else {
            $script:unresolved.Add($name)
            return $undefLine
        }
    })

# config.status 会在首行**追加**一行来源注释（保留 autoheader 那行）
$outText = "/* config.h.  Generated from config.h.in by configure.  */`n" + $outText

$outDir = Split-Path $Out -Parent
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
Set-Content -Path $Out -Value $outText -NoNewline -Encoding ASCII

Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue

if ($unresolved.Count -gt 0) {
    Write-Warning ('未判定的宏（已按未定义写出）: ' + ($unresolved -join ', '))
}
Write-Host ("config.h written to $Out  (defined $definedCount)")
exit 0
