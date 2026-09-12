-- KDU（hfiref0x）—— 用 xmake 构建
--
-- 源码在 `third_party/KDU`（git submodule，指向上游）。上游只提供 MSBuild 工程
-- （Source/Hamakaze/KDU.vcxproj，PlatformToolset v145），本文件复刻它的编译/链接设置，
-- 不改动上游任何源码或工程文件。
--
-- 本工程刻意放在 submodule **之外**：父仓库跟踪这里的脚本，全新克隆时
-- `git submodule update --init --recursive` 之后即可直接构建；否则脚本只存在于
-- submodule 工作区，克隆后就丢了。
--
--   * 目标  kdu.exe（Console 子系统、/MT、MinSpace 优化、UAC requireAdministrator）
--   　　　　drv64.dll（Tanikaze：KDU 的 provider 数据库，含全部 provider 驱动）
--   * 源    Hamakaze 根目录的 *.cpp、idrv/*.cpp、hde/hde64.c、tests/*.cpp，
--           以及 Shared 下的 ldr / minirtl / ntos / thirdparty 子集
--   * 汇编  shellmasm.asm 经 ml64 编译（提供 ZmShellStager / BaseShellDSEFix 等符号）
--   * 资源  resource.rc（版本信息 + 图标 + Taigei 载荷），rc.exe 编译
--   * 后置  Utils/GenAsIo2Unlock 向 exe 注入 AsIo2 unlock 资源并刷新 PE 校验和
--
-- 用法：
--   xmake f -P third_party/KDU.build --yes -p windows -a x64    # 默认 msvc，与上游一致
--   xmake build -P third_party/KDU.build
--   xmake run -P third_party/KDU.build kdu -list                # 用 xmake 直接调用程序
--   xmake run -P third_party/KDU.build kdu -dse 0               # 关闭 DSE（会改内核变量）

set_project("KDU")
set_version("1.5.0")

-- 上游是 MSVC 工程（PlatformToolset v145）。默认用 msvc 工具链以保证行为一致。
set_toolchains("msvc")

-- C++ 目标用 c++17；.asm 由下面 kdu_shellcode 目标单独以 asm 语言处理。
set_languages("c++17", "c11")

add_rules("mode.debug", "mode.release")

-- 上游 Release：<Optimization>MinSpace</Optimization> + <FavorSizeOrSpeed>Size</FavorSizeOrSpeed>
-- + <StringPooling>true</StringPooling>
if is_mode("release") then
    set_optimize("smallest")
    add_cxflags("/Gy", "/GF")
end

-- 上游源码根（submodule 内）
local KDU = "../KDU"
local HAMAKAZE = KDU .. "/Source/Hamakaze"
local SHARED = KDU .. "/Source/Shared"

-- 上游 Release/Debug 均为 <RuntimeLibrary>MultiThreaded</RuntimeLibrary>（静态 CRT）
set_runtimes(is_mode("debug") and "MTd" or "MT")

-- 项目资源文件（resource.rc）里的相对路径按 rc 所在目录解析，这里的 includedirs 也要
-- 覆盖到它需要的头（resource.h / winres.h 等由 rc.exe 自身的包含路径提供）。
local function kdu_common(target)
    target:add("includedirs", HAMAKAZE, KDU .. "/Source", SHARED)
    target:add("defines", "UNICODE", "_UNICODE", "_CONSOLE")
    -- 上游 <BufferSecurityCheck>false</BufferSecurityCheck>
    target:add("cxflags", "/GS-", {force = true})
    -- 上游 <ConformanceMode>true</ConformanceMode>
    target:add("cxflags", "/permissive-", {force = true})
    -- MSBuild 的默认 AdditionalDependencies（上游工程未覆盖它，链接器靠这套默认库）；
    -- 其余库由源码里的 #pragma comment(lib, ...) 带进来。
    target:add("syslinks", "kernel32", "user32", "gdi32", "winspool", "comdlg32", "advapi32",
        "shell32", "ole32", "oleaut32", "uuid", "odbc32", "odbccp32")
end

-- 汇编：shellmasm.asm -> .obj，随 kdu 一起链接。
-- 单独一个 static 目标：这个 .asm 定义的是被 shellcode.cpp 引用的全局符号，
-- 链接器会按符号从静态库里取出该 obj（与 MSBuild 里把 .obj 直接列进链接是等价的）。
target("kdu_shellcode")
    set_kind("static")
    add_files(HAMAKAZE .. "/shellmasm.asm")

target("kdu")
    set_kind("binary")
    on_load(function (target) kdu_common(target) end)

    add_files(HAMAKAZE .. "/*.cpp")
    add_files(HAMAKAZE .. "/idrv/*.cpp")
    add_files(HAMAKAZE .. "/hde/hde64.c")
    add_files(HAMAKAZE .. "/tests/*.cpp")
    add_files(HAMAKAZE .. "/resource.rc")

    add_files(SHARED .. "/ldr/ldr.cpp")
    add_files(SHARED .. "/ntos/ntsup.c")
    add_files(SHARED .. "/thirdparty/tinyaes/aes.c")
    add_files(SHARED .. "/thirdparty/whirlpool/whirlpool.c")
    -- minirtl 子集照抄 vcxproj 的 ClCompile 列表（上游并未编译该目录下全部 .c）
    add_files(
        SHARED .. "/minirtl/cmdline.c",
        SHARED .. "/minirtl/strtou64.c",
        SHARED .. "/minirtl/strtoul.c",
        SHARED .. "/minirtl/u64tohex.c",
        SHARED .. "/minirtl/_filename.c",
        SHARED .. "/minirtl/_strcat.c",
        SHARED .. "/minirtl/_strcmp.c",
        SHARED .. "/minirtl/_strcmpi.c",
        SHARED .. "/minirtl/_strcpy.c",
        SHARED .. "/minirtl/_strend.c",
        SHARED .. "/minirtl/_strlen.c",
        SHARED .. "/minirtl/_strncmp.c",
        SHARED .. "/minirtl/_strncpy.c",
        SHARED .. "/minirtl/_strstri.c"
    )

    add_deps("kdu_shellcode")

    add_ldflags("/subsystem:console", {force = true})
    -- 上游 <UACExecutionLevel>RequireAdministrator</UACExecutionLevel>。
    -- 不要给值加内层双引号：会被转义成 \"…\" 字面量传下去，link.exe 静默忽略。
    add_ldflags("/MANIFESTUAC:level='requireAdministrator'", {force = true})
    -- 必须显式要求嵌入：不加这一条时链接出来的 exe 里没有清单资源。
    add_ldflags("/MANIFEST:EMBED", {force = true})
    -- 上游 <AdditionalOptions>/NOCOFFGRPINFO %(AdditionalOptions)</AdditionalOptions>
    add_ldflags("/NOCOFFGRPINFO", {force = true})

-- 后置工具：向 kdu.exe 注入 AsIo2 unlock 资源并重算 PE 校验和（上游 PostBuildEvent）
target("GenAsIo2Unlock")
    set_kind("binary")
    on_load(function (target) kdu_common(target) end)
    add_files(KDU .. "/Source/Utils/GenAsIo2Unlock/main.cpp")
    add_files(SHARED .. "/minirtl/cmdline.c")

-- Tanikaze -> drv64.dll：KDU 的**外部 provider 数据库**。
--
-- 必须有它：Hamakaze 里那份"内嵌数据库"（Source/Hamakaze/provdb.cpp）是空的
-- （gEmbeddedProvEntry[] = { {} }），真正的 provider 表在 Tanikaze 里。KDU 默认
-- KduDbSourceAuto：先找 drv64.dll，找不到才退回内嵌——而内嵌是空表，于是运行时表现为
-- "Provider: (null)" + "Driver resource id cannot be found 0"，一个驱动都没加载、
-- 没有任何内核写入（`kdu -dse 0` 静默地什么也没做）。上游把两者成对发版。
--
-- 数据库本体 Source/Tanikaze/data/kdu.db 由它作为 RCDATA 嵌进 DLL。
target("drv64")
    set_kind("shared")
    on_load(function (target) kdu_common(target) end)
    add_files(KDU .. "/Source/Tanikaze/main.cpp")
    add_files(KDU .. "/Source/Tanikaze/resource.rc")
    -- 导出 gProvTable / gVersion（KDU 用 GetProcAddress 取这两个符号）。
    -- 必须是 add_shflags 而不是 add_ldflags：xmake 里 shared 目标的链接选项走 shflags
    -- （languages/c/xmake.lua 的 set_targetflags{shared = "shflags"}），写进 ldflags
    -- 会被静默忽略，链接出的 DLL 完全没有导出表。
    add_shflags("/EXPORT:gProvTable", "/EXPORT:gVersion", {force = true})
    add_shflags("/subsystem:windows", {force = true})
    set_basename("drv64")

-- 上游 PostBuildEvent 调用 GenAsIo2Unlock 处理刚链接出来的 kdu.exe。
-- 放在这里（而不是新开一个 target 块）是为了让 kdu 与它的依赖关系集中在一处。
target("kdu")
    add_deps("GenAsIo2Unlock")
    after_build(function (target)
        local tool = path.join(target:targetdir(), "GenAsIo2Unlock.exe")
        if not os.isfile(tool) then
            print("KDU: GenAsIo2Unlock.exe not found, skipping the AsIo2 resource step")
            return
        end
        os.execv(tool, {target:targetfile()})
    end)
