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
-- 注意：调用时 PATH 里必须有 D:\msys64\usr\bin，否则 gcc 驱动 spawn 的 cc1.exe
-- 找不到 msys-2.0.dll，报 "error while loading shared libraries"。
-- 在 MSYS 控制台里直接跑就没有这个问题。
--
-- config.h 是这套源码在本机 configure 出来的 cygwin 配置（WINDOWS 1 / HAVE_SETXATTR 等），
-- 构建时会以 -DHAVE_CONFIG_H 引入。

set_project("ntfs-3g")
set_version("2022.10.3")

-- 上游未指定 -std，用当时的编译器默认值。gcc 15 默认 gnu23，而 C23 取消了隐式函数声明
-- 等宽松规则，2022 年的老代码会大面积报错，因此显式钉到 gnu17。
set_languages("gnu17")

add_rules("mode.debug", "mode.release")

-- libntfs-3g 的源集合照抄 configure 生成的 Makefile：
-- 31 个核心源 + win32_io.c（WINDOWS 分支）；unix_io.c 是另一分支，必须排除。
local INCLUDE_DIRS = {".", "include/ntfs-3g"}

target("ntfs-3g")
    set_kind("static")
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
