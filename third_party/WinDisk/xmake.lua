-- WinDisk —— 内核驱动（WDM）：直发 SCSI 请求到磁盘 miniport，绕过文件系统栈
--
-- 单独一个 xmake 工程：驱动是独立产物（.sys），与 TUI 主程序（icemelt.exe）分开构建。
--
--   xmake f --yes -p windows -a x64 --toolchain=clang-cl
--   xmake build
--
-- 依赖：Windows Driver Kit（本机 "Windows Kits\10"）。
-- xmake 内置 wdk 规则会自行探测 WDK 并注入 km / shared / km/crt 头路径与 km/<arch> 库路径。

set_project("WinDisk")
set_version("1.0.0")

-- 内核模式：C++ 异常与 RTTI 在内核不可用
set_languages("c++17")

add_rules("mode.debug", "mode.release")

local DEFAULT_TOOLCHAIN = "clang-cl"
set_toolchains(DEFAULT_TOOLCHAIN)

-- 目标 Windows 版本（对应 MSBuild 工程的 TargetVersion）。xmake 的 wdk 规则在没有这个值时
-- 会全部落到 Windows 10：_NT_TARGET_VERSION=0x0A00、NTDDI_VERSION=0x0A000000，
-- 并且把 PE 头写成「最低子系统版本 10.00」—— 那样的 .sys 在 Windows 7 上根本不会加载。
--
-- 默认 win7：产出的 .sys 最低子系统版本 6.01，从 Win7 SP1 一直到 Win11 都能加载
-- （低版本目标对高版本系统是兼容的，反向不成立）。冻结还原类软件大量部署在 Win7 机房，
-- 因此这个默认值同时覆盖新旧目标机。
--
-- 覆盖方式（优先级从高到低）：
--   1. xmake f --wdk_winver=win10     （子工程自己的 config，会被根工程的 f 重置）
--   2. ICEMELT_WDK_WINVER=win10       （环境变量，根工程重配也保留）
--   3. 本文件的默认值 win7
--
-- WDK 版本：现役的任何一版都还能面向 Win7 —— 逐个查过实际文件，26100、28000.1839、
-- 28000.2526 都仍然定义 _NT_TARGET_VERSION_WIN7 (0x0601)、把它列在 Valid_NTTARGETVERSIONS 里、
-- 并且带 km/x64/BufferOverflowK.lib；也都有 `Desktop + _NT_TARGET_VERSION < WIN8` 的专门处理。
--
-- 真正会挡住你的是 MSBuild 工程的 DriverTargetPlatform，不是 WDK 版本：
--   * Desktop（默认，经典 WDM/KMDF 工程）  -> Win7 合法
--   * Windows Driver（新工程模型）        -> 强制 _NT_TARGET_VERSION >= RS5，只能 Win10+
-- 本工程不走 MSBuild 那套 props/targets（xmake 直接调 clang-cl/link.exe，自己下发
-- _NT_TARGET_VERSION 等宏），所以那些校验一次都不跑。
--
-- 注意「能用」与「官方支持」的区别：微软的 WDK 支持矩阵里，只有 10.0.19041.5738 标注
-- "Supported for Windows 7/Windows 8/Windows 8.1 driver development only"。
--
-- 构建时可能出现 `linkdir 'Windows Kits\10\Lib\win7\km\x64' not found` 警告 —— 那是 xmake
-- 按版本名追加的库目录（WDK 8.1 时代的布局），本工程不需要，警告无害。
local wdk_winver = get_config("wdk_winver") or os.getenv("ICEMELT_WDK_WINVER") or "win7"
if not get_config("wdk_winver") then
    set_values("wdk.env.winver", wdk_winver)
end

target("WinDisk")
    -- wdk.env.wdm 注入 WDK 头/库路径与内核模式宏；wdk.driver 负责 .sys 命名、
    -- 链接 ntoskrnl/hal/wmilib/ntstrsafe、/kernel /driver /nodefaultlib 与入口点
    add_rules("wdk.env.wdm", "wdk.driver")

    add_files("Main.cpp", "ScsiDisk.cpp", "DataList.cpp", "FileUnlock.cpp", "UsingCPP.cpp")

    -- 原 DDK sources 文件中的 CPP_DEFINES
    add_defines("PNP_POWER")

    -- 源码是 UTF-8（无 BOM）且含中文注释：clang-cl 默认按系统 ANSI 代码页解析
    if get_config("toolchain") == "clang-cl" then
        add_cxflags("/utf-8", { force = true })
    end

    -- 对齐原 vcxproj：<BufferSecurityCheck>false</BufferSecurityCheck>
    -- 开 /GS 会引用 __security_check_cookie，从 BufferOverflowK.lib 拉入 gs_support.obj，
    -- 而该 obj 里的 GsDriverEntry 需要一个「未修饰的 C 符号 DriverEntry」——与 C++ 修饰名冲突。
    add_cxflags("/GS-", { force = true })

    -- 对齐原 vcxproj：<EntryPointSymbol>DriverEntry</EntryPointSymbol>
    -- 必须显式指定，否则 xmake 的 wdk 规则默认用 GsDriverEntry（同上会撞未修饰名）。
    add_ldflags("-entry:DriverEntry", { force = true })

    -- 原工程为 /W3 /WX；clang-cl 诊断面更宽，先不把警告当错误
    add_cxflags("/W3", { force = true })
