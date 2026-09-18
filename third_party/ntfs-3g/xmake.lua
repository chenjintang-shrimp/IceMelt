-- ntfs-3g —— 改造版（含 handle: 设备协议与自研 ntfs-3g-cli）
--
-- 目标平台：MSYS2 的 **mingw-w64**（x86_64-w64-mingw32，msvcrt）。
--
--   cd third_party/ntfs-3g
--   xmake f -P . --yes -p mingw -a x86_64 --toolchain=gcc \
--       --cc=D:/msys64/mingw64/bin/gcc.exe  --cxx=D:/msys64/mingw64/bin/g++.exe \
--       --ld=D:/msys64/mingw64/bin/g++.exe --ar=D:/msys64/mingw64/bin/ar.exe
--   xmake build -P .
--
-- 为什么不再是 MSYS2 的 msys（Cygwin）子系统 —— 那是这条链路在 Windows 7 上
-- 最大的一个阻塞点：cygwin 产物依赖 msys-2.0.dll，而 Cygwin 3.5（MSYS2
-- 2024-05-03）起明确不再支持 Windows 7/8，当前的 msys 包要求 Win10/Server 2016。
-- 于是 ntfscp 在 Win7 上根本起不来，而这正是把 SYSTEM hive 写回磁盘的唯一手段。
--
-- mingw-w64 换成 msvcrt 运行时，默认就面向 Windows 7（_WIN32_WINNT 0x601），
-- 并且静态链接后产物只依赖 kernel32.dll + msvcrt.dll —— 两者 Win7 自带，
-- 整个 tools/ 目录可以直接拷到 Win7 上运行，没有额外运行时 DLL。
--
-- 与 Cygwin 的 POSIX 差异集中放在 mingw-compat/ 里（见该目录下的说明），
-- 上游源码只动了四处、每处都有注释：win32_io.c 的 _get_osfhandle 声明、
-- compat.c 的 daemon() 守卫、compat.h 的 __attribute__ 屏蔽、以及本仓库自有的
-- ntfs-3g-fuse.h / ntfs-3g-cli.c。

set_project("ntfs-3g")
set_version("2022.10.3")

-- 上游未指定 -std，用当时的编译器默认值。gcc 16 默认 gnu23，而 C23 取消了隐式函数声明
-- 等宽松规则，2022 年的老代码会大面积报错，因此显式钉到 gnu17。
set_languages("gnu17")

add_rules("mode.debug", "mode.release")

-- mingw-w64 工具链：任意发行版都行。它是原生 Windows 程序（只依赖系统 DLL 与自己的
-- libwinpthread-1.dll / libgcc_s_seh-1.dll），不依赖 msys-2.0.dll，所以 MSYS2 的
-- mingw64、WinLibs、w64devkit 等等都一样用；把 MINGW_ROOT 指过去即可。
-- 注意要选 **msvcrt** 变体（MSYS2 的 mingw64），不要 ucrt64：UCRT 在 Win7 上需要额外运行库。
local MSYS_ROOT = os.getenv("MSYS_ROOT") or "D:/msys64"
local MINGW_ROOT = os.getenv("MINGW_ROOT") or (MSYS_ROOT .. "/mingw64")
local MINGW_BIN = MINGW_ROOT .. "/bin"

local PROJECT_DIR = os.projectdir()
local MINGW_CONFIG_DIR = PROJECT_DIR .. "/build/mingw"
local MINGW_COMPAT_DIR = PROJECT_DIR .. "/mingw-compat"

-- config.h 由 genconfig.ps1 用 gcc 探测生成（复刻 autoconf 的检查，产出与它逐字节一致）。
-- 这一步**不需要 POSIX shell**：configure 本身是一份 19000 余行的 /bin/sh 脚本，
-- 没有 shell 跑不起来，所以改成用 PowerShell 直接做探测（PS 5.1 与 pwsh 7 都能跑）。
-- config.h 已存在时整个跳过。
local function find_powershell()
    local candidates = {
        "pwsh.exe",                                   -- PowerShell 7+（PATH 里）
        os.getenv("ProgramFiles") .. "/PowerShell/7/pwsh.exe",
        (os.getenv("SystemRoot") or "C:/Windows") ..
            "/System32/WindowsPowerShell/v1.0/powershell.exe",  -- 5.1，恒有
    }
    for _, ps in ipairs(candidates) do
        if ps and (os.isfile(ps) or os.isfile(ps .. ".exe")) then return ps end
    end
    return nil
end

rule("ntfs3g.mingw.config")
    on_load(function (target)
        local config_h = MINGW_CONFIG_DIR .. "/config.h"
        if os.isfile(config_h) then
            return
        end

        local gcc = MINGW_BIN .. "/gcc.exe"
        if not os.isfile(gcc) then
            raise("ntfs-3g: mingw-w64 gcc not found at " .. gcc ..
                  " -- install it with: pacman -S mingw-w64-x86_64-gcc, "
                  .. "or point MINGW_ROOT at any mingw-w64 toolchain")
        end

        local ps = find_powershell()
        if not ps then
            raise("ntfs-3g: PowerShell not found (looked for pwsh and " ..
                  "System32/WindowsPowerShell/v1.0/powershell.exe); it is needed to probe " ..
                  "the toolchain and write config.h")
        end

        print("[ntfs-3g] generating config.h (one-time, via " .. ps .. ")")
        local script = PROJECT_DIR .. "/genconfig.ps1"
        local code = os.execv(ps, {"-NoProfile", "-ExecutionPolicy", "Bypass",
                                   "-File", script,
                                   "-Gcc", gcc,
                                   "-Out", config_h})
        assert(code == 0, "ntfs-3g: genconfig.ps1 failed, exit code " .. tostring(code))
        assert(os.isfile(config_h), "ntfs-3g: genconfig.ps1 did not produce config.h")
    end)

-- 公共设置：mingw 的 config.h 在 build/mingw/，而**不能**把源码根目录放进
-- include 路径 —— 那里有一份 cygwin 构建用的 config.h，会先被找到。
local function mingw_common(target)
    target:add("includedirs", MINGW_CONFIG_DIR, MINGW_COMPAT_DIR, "include/ntfs-3g")
    target:add("defines", "HAVE_CONFIG_H")
    -- mingw-compat 里的补充声明必须在所有头之前生效（详见该头文件注释）
    target:add("cxflags", "-include " .. MINGW_COMPAT_DIR .. "/ntfs_mingw_compat.h")
    -- 静态链接：否则 mingw 会给用到底层 pthread 的产物挂上 libwinpthread-1.dll
    -- （ntfs-3g 的日志代码里有互斥量），那又变成一个 Win7 上不存在的依赖。
    -- 静态化之后依赖只剩 kernel32.dll + msvcrt.dll，两者 Win7 自带。
    target:add("ldflags", "-static", {force = true})
end

-- libntfs-3g 的源集合照抄 configure 生成的 Makefile：
-- 30 个核心源 + win32_io.c（WINDOWS 分支）；unix_io.c 是另一分支，必须排除。
-- 外加 mingw-compat/mingw_compat.c（pwd.h/grp.h 那几个查不到用户组的桩实现）。
target("ntfs-3g")
    set_kind("static")
    add_rules("ntfs3g.mingw.config")
    -- 显式枚举而不是 `add_files("libntfs-3g/*.c", {excludes = ...})`：
    -- xmake 的 excludes 是 Lua 模式、比对的是内部路径字符串，实测 "unix_io.c" 与
    -- "libntfs-3g/unix_io.c" 都命中不了（后者里的 `-` 还会被当成量词）。
    add_files(
        "libntfs-3g/acls.c", "libntfs-3g/attrib.c", "libntfs-3g/attrlist.c",
        "libntfs-3g/bitmap.c", "libntfs-3g/bootsect.c", "libntfs-3g/cache.c",
        "libntfs-3g/collate.c", "libntfs-3g/compat.c", "libntfs-3g/compress.c",
        "libntfs-3g/debug.c", "libntfs-3g/device.c", "libntfs-3g/dir.c",
        "libntfs-3g/ea.c", "libntfs-3g/efs.c", "libntfs-3g/index.c",
        "libntfs-3g/inode.c", "libntfs-3g/ioctl.c", "libntfs-3g/lcnalloc.c",
        "libntfs-3g/logfile.c", "libntfs-3g/logging.c", "libntfs-3g/mft.c",
        "libntfs-3g/misc.c", "libntfs-3g/mst.c", "libntfs-3g/object_id.c",
        "libntfs-3g/realpath.c", "libntfs-3g/reparse.c", "libntfs-3g/runlist.c",
        "libntfs-3g/security.c", "libntfs-3g/unistr.c", "libntfs-3g/volume.c",
        "libntfs-3g/win32_io.c", "libntfs-3g/xattrs.c"
    )
    add_files("mingw-compat/mingw_compat.c")
    on_load(function (target) mingw_common(target) end)

target("ntfsfix")
    set_kind("binary")
    add_files("ntfsprogs/ntfsfix.c", "ntfsprogs/utils.c")
    add_deps("ntfs-3g")
    add_includedirs("ntfsprogs")
    on_load(function (target) mingw_common(target) end)

target("ntfscp")
    set_kind("binary")
    add_files("ntfsprogs/ntfscp.c", "ntfsprogs/utils.c")
    add_deps("ntfs-3g")
    add_includedirs("ntfsprogs")
    on_load(function (target) mingw_common(target) end)

-- 造 test 卷用：把 NTFS 建在一个普通文件里，从而能在不碰裸盘、不装驱动的前提下
-- 用同一份 libntfs-3g 复现"写入后 initialized_size 不对"的问题（隔离出驱动变量）。
-- mingw 的 struct stat 没有 st_blocks（msvcrt 就没有这个字段），而 mkntfs 只用它做
-- "size 为 0 时的兜底"；把 st_blocks 映射到 st_size 语义完全等价（该分支仅在
-- st_size == 0 时可达，此时两者都是 0），且不必改第三方源码。
target("mkntfs")
    set_kind("binary")
    add_files("ntfsprogs/mkntfs.c", "ntfsprogs/attrdef.c", "ntfsprogs/boot.c",
              "ntfsprogs/sd.c", "ntfsprogs/utils.c")
    add_deps("ntfs-3g")
    add_includedirs("ntfsprogs")
    add_cxflags("-Dst_blocks=st_size")
    on_load(function (target) mingw_common(target) end)

-- 自研 CLI：用管道协议驱动 libntfs-3g 在 NTFS 上做文件操作。
-- ntfs_fuse_* 接口在 src/ntfs-3g-fuse.c，与 ntfs-3g-cli.c 一起构成该工具。
target("ntfs-3g-cli")
    set_kind("binary")
    add_files("src/ntfs-3g-cli.c", "src/ntfs-3g-fuse.c")
    add_deps("ntfs-3g")
    add_includedirs("src")
    on_load(function (target) mingw_common(target) end)
