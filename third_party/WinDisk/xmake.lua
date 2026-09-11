-- WinDisk —— 内核驱动（WDM）：直发 SCSI 请求到磁盘 miniport，绕过文件系统栈
--
-- 单独一个 xmake 工程：驱动是独立产物（.sys），与 TUI 主程序（secmelt.exe）分开构建。
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
