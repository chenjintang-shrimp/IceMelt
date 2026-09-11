-- ntfs-3g —— 改造版（含 handle: 设备协议与自研 ntfs-3g-cli）
--
-- 目标平台是 MSYS2 的 msys 子系统（即 Cygwin 兼容环境），不是 ucrt64/mingw64。
-- 必须用 D:\msys64\usr\bin 下的 cygwin 版 gcc（target: x86_64-pc-cygwin）。
--
--   cd third_party/ntfs-3g
--   xmake f -P . --yes -p msys -a x86_64 --toolchain=gcc \
--       --cc=D:/msys64/usr/bin/gcc.exe  --cxx=D:/msys64/usr/bin/g++.exe \
--       --ld=D:/msys64/usr/bin/g++.exe --ar=D:/msys64/usr/bin/ar.exe
--   xmake build -P .
--
-- 两个必须显式指定 cc/cxx/ld/ar 的理由：否则 xmake 用 PATH 里第一个 gcc，
-- 若 ucrt64 排在 msys64/usr/bin 之前就会静默选到 mingw 版；同时调用时 PATH 需含
-- D:\msys64\usr\bin，否则 gcc 驱动 spawn 的 cc1.exe 找不到 msys-2.0.dll。
-- 在 MSYS 控制台里直接跑则无此问题。

set_project("ntfs-3g")
set_version("2022.10.3")

-- 上游未指定 -std，用当时的编译器默认值。gcc 15 默认 gnu23，而 C23 取消了隐式函数声明
-- 等宽松规则，2022 年的老代码会大面积报错，因此显式钉到 gnu17。
set_languages("gnu17")

add_rules("mode.debug", "mode.release")

-- MSYS2 安装根（msys 子系统）。可用环境变量 MSYS_ROOT 覆盖。
local MSYS_ROOT = os.getenv("MSYS_ROOT") or "D:/msys64"
local MSYS_BASH = MSYS_ROOT .. "/usr/bin/bash.exe"

-- D:/foo -> /d/foo（msys 形式）
local function to_msys(p)
    local drive, rest = p:match("^(%a):[/\\](.*)$")
    if drive then
        return "/" .. drive:lower() .. "/" .. rest:gsub("\\", "/")
    end
    return (p:gsub("\\", "/"))
end

-- config.h 由 autoconf 的 configure 生成，构建必需（-DHAVE_CONFIG_H）。
--
-- 调用方式不能省：必须 --no-create --no-recursion 之后再单独跑
-- `./config.status config.h`。直接 `./configure` 会在 AC_OUTPUT 阶段触发
-- config.status --recheck（把 configure 重跑一遍），而那次重跑产出的 config.h
-- 与其自身探测结果不一致（实测：日志里 ac_cv_c_bigendian=no，config.h 里
-- WORDS_LITTLEENDIAN 却是 #undef），会让 libntfs-3g/dir.c 的 index_union
-- 类型错乱、编译直接失败。
rule("ntfs3g.config")
    on_load(function (target)
        local dir = target:scriptdir()
        local config_h = dir .. "/config.h"
        if os.isfile(config_h) then
            return
        end

        if not os.isfile(MSYS_BASH) then
            raise("ntfs-3g: bash not found at " .. MSYS_BASH ..
                  " -- set MSYS_ROOT to your MSYS2 install root")
        end

        print("[ntfs-3g] config.h missing; generating via ./configure")
        local cmd = table.concat({
            "cd " .. to_msys(dir),
            "./configure --disable-ntfs-3g --no-create --no-recursion",
            "./config.status config.h",
        }, " && ")

        local code = os.execv(MSYS_BASH, {"-lc", cmd})
        assert(code == 0, "ntfs-3g: configure failed, exit code " .. tostring(code))
        assert(os.isfile(config_h), "ntfs-3g: configure did not produce config.h")
    end)

-- libntfs-3g 的源集合照抄 configure 生成的 Makefile：
-- 31 个核心源 + win32_io.c（WINDOWS 分支）；unix_io.c 是另一分支，必须排除。
local INCLUDE_DIRS = {".", "include/ntfs-3g"}

target("ntfs-3g")
    set_kind("static")
    add_rules("ntfs3g.config")
    add_files("libntfs-3g/*.c", {excludes = "libntfs-3g/unix_io.c"})
    add_includedirs(INCLUDE_DIRS)
    add_defines("HAVE_CONFIG_H")

target("ntfsfix")
    set_kind("binary")
    add_files("ntfsprogs/ntfsfix.c", "ntfsprogs/utils.c")
    add_includedirs(INCLUDE_DIRS, "ntfsprogs")
    add_defines("HAVE_CONFIG_H")
    add_deps("ntfs-3g")

target("ntfscp")
    set_kind("binary")
    add_files("ntfsprogs/ntfscp.c", "ntfsprogs/utils.c")
    add_includedirs(INCLUDE_DIRS, "ntfsprogs")
    add_defines("HAVE_CONFIG_H")
    add_deps("ntfs-3g")

-- 自研 CLI：用管道协议驱动 libntfs-3g 在 NTFS 上做文件操作。
-- ntfs_fuse_* 接口在 src/ntfs-3g-fuse.c，与 ntfs-3g-cli.c 一起构成该工具。
target("ntfs-3g-cli")
    set_kind("binary")
    add_files("src/ntfs-3g-cli.c", "src/ntfs-3g-fuse.c")
    add_includedirs(INCLUDE_DIRS, "src")
    add_defines("HAVE_CONFIG_H")
    add_deps("ntfs-3g")
