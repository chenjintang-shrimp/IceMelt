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

-- MSYS2 安装根：mingw-w64 工具链与（生成 config.h 用的）bash 都在这里。
-- 可用 MSYS_ROOT 覆盖。
local MSYS_ROOT = os.getenv("MSYS_ROOT") or "D:/msys64"
local MSYS_BASH = MSYS_ROOT .. "/usr/bin/bash.exe"
local MINGW_BIN = MSYS_ROOT .. "/mingw64/bin"

-- D:/foo -> /d/foo（msys 形式）
local function to_msys(p)
    local drive, rest = p:match("^(%a):[/\\](.*)$")
    if drive then
        return "/" .. drive:lower() .. "/" .. rest:gsub("\\", "/")
    end
    return (p:gsub("\\", "/"))
end

local PROJECT_DIR = os.projectdir()
local MINGW_CONFIG_DIR = PROJECT_DIR .. "/build/mingw"
local MINGW_COMPAT_DIR = PROJECT_DIR .. "/mingw-compat"

-- config.h 由 autoconf 的 configure 生成，构建必需（-DHAVE_CONFIG_H）。
--
-- 生成方式有两处不能简化，都是实测踩出来的：
--
-- 1. configure 必须在**源码副本**里跑。configure 拒绝在"源目录已就地配置过"的
--    情况下做 out-of-tree 构建（会报 "source directory already configured"），
--    而 in-tree 跑又会把 config.h 写进源码根目录、与构建脚本共享同一份。
--    所以复制一份源码树、在其中生成 config.h，再把它取出来放到 build/mingw/，
--    副本随即删除。产物目录只留一个 config.h。
--
-- 2. 必须 --no-create --no-recursion 之后单独跑 `./config.status config.h`。
--    直接 `./configure` 会在 AC_OUTPUT 阶段触发 config.status --recheck（把
--    configure 重跑一遍），而那次重跑产出的 config.h 与其自身探测结果不一致
--    （实测：日志里 ac_cv_c_bigendian=no，config.h 里 WORDS_LITTLEENDIAN 却是
--    #undef），会让 libntfs-3g/dir.c 的 index_union 类型错乱、编译直接失败。
--
-- 另外三个 configure 参数/覆盖也是必需的：
--   --disable-plugins       插件要 dlopen/libdl，mingw 没有（dlopen 只用在
--                           src/ntfs-3g_common.c，本工程不编译它）
--   ac_cv_c_bigendian=no    交叉编译模式下 AC_C_BIGENDIAN 无法运行测试程序
--   ac_cv_header_libintl_h=no
--                           mingw64 装有 gettext 的 libintl.h，一旦探测到它，
--                           utils.c 就会 include <libintl.h>，而该头会把
--                           printf/setlocale/snprintf 重定向到 libintl_*，
--                           从而凭空多出一个 libintl DLL 依赖。cygwin 那份
--                           config.h 同样没有 HAVE_LIBINTL_H，这里保持一致。
rule("ntfs3g.mingw.config")
    on_load(function (target)
        local config_h = MINGW_CONFIG_DIR .. "/config.h"
        if os.isfile(config_h) then
            return
        end

        if not os.isfile(MSYS_BASH) then
            raise("ntfs-3g: bash not found at " .. MSYS_BASH ..
                  " -- set MSYS_ROOT to your MSYS2 install root")
        end
        if not os.isfile(MINGW_BIN .. "/gcc.exe") then
            raise("ntfs-3g: mingw-w64 gcc not found at " .. MINGW_BIN ..
                  "/gcc.exe -- install it with: pacman -S mingw-w64-x86_64-gcc")
        end

        print("[ntfs-3g] generating a mingw-w64 config.h (one-time)")
        local scratch = PROJECT_DIR .. "/build/mingw-src"
        local cmd = table.concat({
            "export PATH=" .. to_msys(MINGW_BIN) .. ":$PATH",
            "rm -rf " .. to_msys(scratch) .. " && mkdir -p " .. to_msys(scratch),
            "cd " .. to_msys(PROJECT_DIR),
            "tar -cf - --exclude=build --exclude=.xmake --exclude=.libs " ..
                "--exclude=.deps --exclude='*.o' --exclude='*.lo' --exclude='*.a' . " ..
                "| (cd " .. to_msys(scratch) .. " && tar -xf -)",
            "rm -f " .. to_msys(scratch) .. "/config.h " .. to_msys(scratch) ..
                "/config.status " .. to_msys(scratch) .. "/stamp-h1",
            "cd " .. to_msys(scratch),
            "ac_cv_c_bigendian=no ac_cv_header_libintl_h=no " ..
                "CC=x86_64-w64-mingw32-gcc AR=ar RANLIB=ranlib " ..
                "./configure --host=x86_64-w64-mingw32 --disable-ntfs-3g " ..
                "--disable-plugins --no-create --no-recursion > configure.log 2>&1",
            "./config.status config.h >> configure.log 2>&1",
            "mkdir -p " .. to_msys(MINGW_CONFIG_DIR),
            "cp config.h " .. to_msys(MINGW_CONFIG_DIR) .. "/config.h",
            "cd " .. to_msys(PROJECT_DIR) .. " && rm -rf " .. to_msys(scratch),
        }, " && ")

        local code = os.execv(MSYS_BASH, {"-lc", cmd})
        assert(code == 0, "ntfs-3g: mingw configure failed, exit code " .. tostring(code))
        assert(os.isfile(config_h), "ntfs-3g: configure did not produce config.h")
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
    -- 源集合照抄 configure 生成的 Makefile：31 个核心源（含 WINDOWS 分支的
    -- win32_io.c），**不含 unix_io.c**。
    --
    -- 这里显式列出而不是用 `add_files("libntfs-3g/*.c", {excludes = ...})`：
    -- xmake 的 excludes 是 Lua 模式，且比对的是内部路径字符串，实测
    -- "unix_io.c" 与 "libntfs-3g/unix_io.c" 都没能命中（"libntfs-3g/unix_io.c"
    -- 里的 `-` 还会被当成量词）。之前 cygwin 那份 xmake.lua 就是这么写的，
    -- 于是 unix_io.c 一直被编进归档却无人察觉（cygwin 有 fsync/fcntl/flock，
    -- 编得过；mingw 编不过才暴露）。枚举没有这层歧义。
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

-- 自研 CLI：用管道协议驱动 libntfs-3g 在 NTFS 上做文件操作。
-- ntfs_fuse_* 接口在 src/ntfs-3g-fuse.c，与 ntfs-3g-cli.c 一起构成该工具。
target("ntfs-3g-cli")
    set_kind("binary")
    add_files("src/ntfs-3g-cli.c", "src/ntfs-3g-fuse.c")
    add_deps("ntfs-3g")
    add_includedirs("src")
    on_load(function (target) mingw_common(target) end)
