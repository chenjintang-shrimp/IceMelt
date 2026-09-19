-- IceMelt —— UnFreeze 后继：基于磁盘级绕过的“解冻”工具链
--
-- 工具链：clang-cl（MSVC ABI 下的 Clang，需本机 Visual Studio + LLVM）
--
-- xmake v3 约束（实测差异）：
--   * add_requires 必须在根作用域，放进 target() 会硬报错
--   * 没有 check_cxflags / import / os.run，无法在 xmake.lua 里探测 flag 支持
--   * 工具链判定走 get_config("toolchain")

-- xmake pack 的 DSL（xpack()）。只有打包时才用得到，但必须在这里 include：
-- 不 include 的话 xmake.lua 里写 xpack() 会直接报 nil。
includes("@builtin/xpack")

set_project("IceMelt")

-- 版本：本文件固定 0.0.0（开发/自建态）。发版时由 CI 在 configure 阶段临时覆盖：
--     xmake f --app_version=1.0.0      # tag 去掉前缀 v 的纯数字三段
-- 它是唯一的版本源，同时决定两处：exe 的版本资源（resource/version-*.rc.in）与
-- xmake pack 的包名（IceMelt-v<版本>-cli / IceMelt-GUI-v<版本>-gui）。
--
-- 注意用字典型声明：xmake 的 DSL 作用域是"粘"的，`option("x") ... end` 那种块写法
-- 之后，根作用域的函数（set_project/set_version/...）会解析成 nil 直接报错。
option("app_version", {
    default = "0.0.0",
    description = "覆盖项目版本（发版时传 tag 去掉前缀 v）；默认 0.0.0"
})

local APP_VERSION = get_config("app_version")
if not APP_VERSION or APP_VERSION == "" then
    APP_VERSION = "0.0.0"
end
set_version(APP_VERSION)

-- 版本资源：模板生成到 $(builddir)/version-{cli,gui}.rc，再由两个前端各自编译进自己的
-- exe（图形前端还有一份 manifest rc，两者各管一件事）。变量名是 xmake 内置的
-- @VERSION@ / @VERSION_MAJOR@ / @VERSION_MINOR@ / @VERSION_ALTER@（第三段叫 ALTER，
-- 没有 PATCH），且必须显式给 pattern —— 默认只替换 ${VAR} 语法。
add_configfiles("resource/version-cli.rc.in", {pattern = "@([%w_]+)@"})
add_configfiles("resource/version-gui.rc.in", {pattern = "@([%w_]+)@"})

-- 整体以 GPLv3 发布。third_party/ntfs-3g 为 GPL-2.0-or-later（允许升到 v3），
-- third_party/KDU 为 MIT（可并入 GPLv3 作品）。见 LICENSE 与 README 的许可说明。
set_license("GPL-3.0-or-later")

set_languages("c++20")

add_rules("mode.debug", "mode.release")

-- 命令行 --toolchain 优先于本文件默认值；两者要一起看，否则 /utf-8 会静默丢失
local DEFAULT_TOOLCHAIN = "clang-cl"
set_toolchains(DEFAULT_TOOLCHAIN)

local function using_clang_cl()
    local configured = get_config("toolchain")
    if configured and configured ~= "" then
        return configured == "clang-cl"
    end
    return DEFAULT_TOOLCHAIN == "clang-cl"
end

-- Dear ImGui（xmake-repo，MIT）：图形前端。后端 Win32 + D3D9 —— D3D9 从 Vista 起
-- 随系统提供，Win7 SP1 → Win11 都预装，无任何可再发行组件；1.92 起动态字形加载，
-- CJK 不需要预烘焙 glyph ranges。
add_requires("imgui v1.92.9", {configs = {dx9 = true, win32 = true}})

-- ============================================================================
-- 一键构建：third_party 下的三个子工程各有自己的工具链，无法并进同一个 xmake
-- 工程（内核 WDK 规则 / msys-cygwin 的 gcc / clang-cl 用户态程序），因此这里在
-- icemelt 链接完成后依次驱动它们，再把产物搬到 exe 旁边。
--
--   xmake build            构建全部（本程序 + 驱动 + ntfs-3g 工具 + KDU）
--   xmake build -P .       同上；子工程不可用时跳过并打印原因，不静默
-- ============================================================================
local XMAKE = os.programfile()
local BUILD_MODE = get_config("mode") or "release"
local ROOT = os.projectdir()
-- mingw-w64 工具链根：默认取 MSYS2 的 mingw64 目录，任意发行版都可以（WinLibs、w64devkit…）。
-- 它是原生 Windows 工具链，不依赖 msys-2.0.dll；要选 msvcrt 变体而不是 ucrt64。
-- MSYS_ROOT 在这里只是"默认前缀"，配置 ntfs-3g 的 config.h 不再需要 POSIX shell。
local MSYS_ROOT = os.getenv("MSYS_ROOT") or "D:/msys64"
local MINGW_ROOT = os.getenv("MINGW_ROOT") or path.join(MSYS_ROOT, "mingw64")
local MINGW_BIN = path.join(MINGW_ROOT, "bin")
-- Windows Kits 10 根：xmake 的 wdk 规则就是从这里取内核头/库的
local WIN_KITS = os.getenv("WindowsSdkDir") or "C:/Program Files (x86)/Windows Kits/10"


-- ============================================================================
-- 三方子工程 + 运行期资产落位：icemelt 与 icemelt-gui 共用这一段 after_build。
--
-- 单独构建任一个前端都应得到完整的运行期组合（驱动 / ntfs-3g 工具 / KDU / 名单），
-- 所以两个 target 挂同一个回调。子工程在一个 xmake 进程里只构建一次（上面的去重
-- flag，两个目标一起构建时不重复跑）；"复制到 exe 旁边"每次都做，幂等且便宜。
--
-- 注意：xmake 的沙箱按 chunk 分配权限 —— 顶层 chunk 里没有 os.execv，只有回调
-- （after_build 等）执行期间才有。所以这个函数只能注册为 after_build 使用。
-- ============================================================================
local third_party_built = false

local function stage_third_party_and_assets(target)
    -- 注意：xmake 的沙箱按 chunk 分配权限 —— 顶层 chunk 里没有 os.execv，
    -- 只有回调（如 after_build）内部才有。所以这个 helper 必须定义在这里。
    local function sub_build(spec)
        local dir = path.join(ROOT, spec.dir)
        if not os.isdir(dir) then
            print("IceMelt: skip " .. spec.label .. " (" .. spec.dir .. " is missing)")
            return
        end
        -- 前置缺失时打印原因并跳过，而不是让整个 xmake build 失败：
        -- 这些产物各自服务于链路的一小段，缺了会在运行日志里明确报出来。
        for _, need in ipairs(spec.needs or {}) do
            local file = need.path
            if not path.is_absolute(file) then file = path.join(ROOT, file) end
            -- 前置可以是文件或目录（如 WDK 的 Include）
            if not (os.isfile(file) or os.isdir(file)) then
                print("IceMelt: skip " .. spec.label .. " (" .. need.what .. " missing: " .. file .. ")")
                return
            end
        end
        local configure = {"f", "-P", dir, "--yes", "-m", BUILD_MODE}
        os.execv(XMAKE, table.join(configure, spec.configure), {curdir = dir})
        os.execv(XMAKE, {"build", "-P", dir}, {curdir = dir, addenvs = spec.envs,
                                               setenvs = spec.setenvs})
    end

-- 三方子工程在一个 xmake 进程里只构建一次（icemelt + icemelt-gui 同时构建时去重）
if not third_party_built then
    third_party_built = true
        -- 先构建第三方产物，再把它们搬到 exe 旁边
        sub_build({
            label = "WinDisk driver",
            dir = "third_party/WinDisk",
            needs = {
                {path = "third_party/WinDisk/xmake.lua", what = "driver project"},
                -- WDK 只能探测到本机 Windows Kits 10；没有就跳过（驱动是裸盘功能的硬前提）
                {path = path.join(WIN_KITS, "Include"), what = "Windows Driver Kit"},
            },
            configure = {"-p", "windows", "-a", "x64", "--toolchain=" .. tostring(get_config("toolchain") or DEFAULT_TOOLCHAIN)},
            -- 驱动的目标系统版本：默认 win7（产出的 .sys 最低子系统版本 6.01，Win7 SP1 →
            -- Win11 都能加载）。只面向 Win10+ 时用 ICEMELT_WDK_WINVER=win10。
            -- 这里显式转发而不是依赖环境继承，使"构建出的是哪个目标版本"在脚本里可见。
            setenvs = {ICEMELT_WDK_WINVER = os.getenv("ICEMELT_WDK_WINVER") or "win7"},
        })
        sub_build({
            label = "ntfs-3g tools",
            dir = "third_party/ntfs-3g",
            -- 目标平台是 mingw-w64（不是 msys/cygwin）：cygwin 产物依赖 msys-2.0.dll，
            -- 而 Cygwin 3.5 起不再支持 Win7 —— 那样 ntfscp 在 Win7 上起不来，而它
            -- 正是把 hive 写回磁盘的唯一手段。mingw-w64 静态链接后只依赖
            -- kernel32/msvcrt，Win7 自带。
            needs = {{path = path.join(MINGW_BIN, "gcc.exe"), what = "mingw-w64 gcc"}},
            configure = {
                "-p", "mingw", "-a", "x86_64", "--toolchain=gcc",
                "--cc=" .. path.join(MINGW_BIN, "gcc.exe"),
                "--cxx=" .. path.join(MINGW_BIN, "g++.exe"),
                "--ld=" .. path.join(MINGW_BIN, "g++.exe"),
                "--ar=" .. path.join(MINGW_BIN, "ar.exe"),
            },
            -- 构建期 mingw 工具链要从 PATH 找到自己的 DLL；config.h 那一步还需要一个
            -- POSIX shell（子工程会自己找 MSYS2 或 Git for Windows 的 bash）。
            envs = {PATH = MINGW_BIN},
            setenvs = {MSYS_ROOT = MSYS_ROOT, MINGW_ROOT = MINGW_ROOT},
        })
        sub_build({
            label = "KDU",
            dir = "third_party/KDU.build",
            -- 构建脚本由父仓库跟踪（源码是 submodule）；缺失即跳过
            needs = {{path = "third_party/KDU.build/xmake.lua", what = "KDU build script"}},
            configure = {"-p", "windows", "-a", "x64"},
        })

end

    -- ---- 产物落位：让输出目录自成一套可直接运行的组合 --------------------
    local outdir = target:targetdir()

    -- 目标名单：复制到输出目录，使运行时紧邻 exe（LoadTargets 先查 <exeDir>/targets.txt）
    os.cp(path.join(ROOT, "config/targets.txt"), path.join(outdir, "targets.txt"))

    local sys = path.join(ROOT, "third_party/WinDisk/build/windows/x64", BUILD_MODE, "WinDisk.sys")
    if os.isfile(sys) then
        os.cp(sys, path.join(outdir, "WinDisk_x64.sys"))
    else
        print("IceMelt: WARNING driver not found at " .. sys .. " (raw disk features will be unavailable)")
    end

    local tools = {"ntfsfix.exe", "ntfscp.exe", "ntfs-3g-cli.exe", "mkntfs.exe"}
    local toolsdir = path.join(outdir, "tools")
    for _, name in ipairs(tools) do
        -- mingw-w64 产物（原来是 msys/x86_64——那是 cygwin 目标，Win7 用不了）
        local tool = path.join(ROOT, "third_party/ntfs-3g/build/mingw/x86_64", BUILD_MODE, name)
        if os.isfile(tool) then
            os.mkdir(toolsdir)
            os.cp(tool, path.join(toolsdir, name))
        else
            print("IceMelt: WARNING ntfs-3g tool not found at " .. tool ..
                  " (raw NTFS write-back will be unavailable)")
        end
    end
    -- 不再需要随包分发 msys-2.0.dll：mingw 产物静态链接，只依赖 Win7 自带的
    -- kernel32.dll + msvcrt.dll（可用 dumpbin /dependents 复核）。
    local stale = path.join(toolsdir, "msys-2.0.dll")
    if os.isfile(stale) then
        os.rm(stale)
    end

    local kdu = path.join(ROOT, "third_party/KDU.build/build/windows/x64", BUILD_MODE, "kdu.exe")
    if os.isfile(kdu) then
        os.cp(kdu, path.join(outdir, "kdu.exe"))
    end
    -- drv64.dll 必须与 kdu.exe 同目录：它是 KDU 的 provider 数据库（含全部 provider
    -- 驱动）。缺了它 kdu 会用那份空的内嵌表，表现为 "Provider: (null)" +
    -- "Driver resource id cannot be found"，然后**静默地什么都不做**（-dse 也不会生效）。
    local drv64 = path.join(ROOT, "third_party/KDU.build/build/windows/x64", BUILD_MODE,
                            "drv64.dll")
    if os.isfile(drv64) then
        os.cp(drv64, path.join(outdir, "drv64.dll"))
    else
        print("IceMelt: WARNING drv64.dll not found at " .. drv64 ..
              " -- kdu will not be able to load any provider")
    end
end

-- ============================================================================
-- 运行期资产（exe 旁边那一套组合）：after_build 把它们从三方产物复制到目标目录，
-- 下面这份清单再把它们登记成"安装文件"—— xmake pack 出的 zip 因此自带完整组合，
-- `xmake install` 也能得到同一套布局。清单与 after_build 的落位一一对应，改一处要改两处。
-- ============================================================================
local RUNTIME_STAGE = "$(builddir)/$(plat)/$(arch)/$(mode)"
local RUNTIME_ASSETS = {"WinDisk_x64.sys", "kdu.exe", "drv64.dll", "targets.txt"}

local function install_runtime_assets()
    for _, name in ipairs(RUNTIME_ASSETS) do
        add_installfiles(RUNTIME_STAGE .. "/" .. name)
    end
    add_installfiles(RUNTIME_STAGE .. "/tools/*.exe", {prefixdir = "tools"})
end

target("icemelt")
    set_kind("binary")
    -- 显式列目录而不是 src/**.cpp：src/gui 是独立 target（有自己的入口点，且要
    -- imgui 依赖），被递归收集进来会让 CLI 目标去编译 GUI 入口。
    add_files(
        "src/*.cpp",
        "src/raw/*.cpp",
        "src/reg/*.cpp",
        "src/melt/*.cpp"
    )

    -- 版本资源（生成的 rc）：没有它，exe 的"属性 → 详细信息"里就没有版本号。
    -- always_added：配置阶段这个文件还没生成，靠它才能进文件列表。
    add_files("$(builddir)/version-cli.rc", {always_added = true})

    -- 双击即用的两个入口，随 CLI 包分发（内容见仓库根的 preflight.bat / melt.bat）
    add_installfiles("preflight.bat", "melt.bat")

    install_runtime_assets()

    -- 源码按 reg/ raw/ melt/ 分目录，内部一律用 "目录/头文件.h" 形式互相引用，
    -- 因此 src 本身要在搜索路径上。
    add_includedirs("src", "third_party")

    -- clang-cl 需要 /utf-8 才能正确解析源码中的 UTF-8 字面量
    if using_clang_cl() then
        add_cxflags("/utf-8", { force = true })
    end

    if is_plat("windows") then
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("advapi32", "shell32", "user32", "rpcrt4")
    end

    after_build(stage_third_party_and_assets)

-- ============================================================================
-- 图形前端：Dear ImGui + Win32 + Direct3D9（设计取舍见 src/gui/gui_main.cpp 头部）
--
-- 与 CLI 共用同一份 pipeline 源码（src/{raw,reg,melt} + util.cpp），只排除两个
-- 入口：main.cpp（命令行前端）与 selftest.cpp（子命令包装）。产物与
-- icemelt.exe 同目录；after_build 与 icemelt 共用同一段落位逻辑，因此
-- `xmake build icemelt-gui` 单独构建时同样会产出 WinDisk.sys / ntfs-3g 工具 /
-- KDU / targets.txt，并复制到本目标的输出目录旁边。
-- ============================================================================
target("icemelt-gui")
    set_kind("binary")
    add_files(
        "src/util.cpp",
        "src/raw/*.cpp",
        "src/reg/*.cpp",
        "src/melt/*.cpp",
        "src/gui/*.cpp"
    )
    -- manifest 的取舍放在构建脚本层，不依赖 define 转发到 rc.exe（实测不可靠：
    -- debug 下 add_defines 的宏没有到达 rc，仍然嵌入了 requireAdministrator）
    if is_mode("debug") then
        -- 宿主开发机的渲染冒烟用：不弹 UAC。release 始终 requireAdministrator
        add_files("src/gui/icemelt_asinvoker.rc")
    else
        add_files("src/gui/icemelt.rc")
    end

    -- 版本资源（生成的 rc），与上面的 manifest rc 各管一件事
    add_files("$(builddir)/version-gui.rc", {always_added = true})

    install_runtime_assets()

    add_packages("imgui")

    -- src/gui 同时进 rc.exe 的 /I：RT_MANIFEST 里的文件名按包含路径解析
    add_includedirs("src", "third_party", "src/gui")

    -- 与 icemelt 同一段落位逻辑：构建本目标即得到完整运行期组合
    after_build(stage_third_party_and_assets)

    if using_clang_cl() then
        add_cxflags("/utf-8", { force = true })
    end

    if is_plat("windows") then
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        -- Win7 最低线写死：头文件层面把 Win8+ API 挡在编译期（STL 的 CreateFile2
        -- 路径因此进不来），子系统版本同时钉 6.01
        add_defines("_WIN32_WINNT=0x0601", "WINVER=0x0601")
        add_syslinks("advapi32", "shell32", "user32", "gdi32", "rpcrt4",
                     "d3d9", "imm32", "dwmapi")
        add_ldflags("/subsystem:windows,6.01")
    end

-- ============================================================================
-- 发布包：xmake pack -f zip -o dist
--
--   IceMelt-v<版本>-cli.zip        命令行前端：icemelt.exe + preflight.bat / melt.bat
--   IceMelt-GUI-v<版本>-gui.zip    图形前端：icemelt-gui.exe
--
-- 两个包都自带一份完整的运行期资产（驱动 / kdu / 名单 / ntfs-3g 工具）—— 内容取自
-- 各 target 的安装文件清单（见 install_runtime_assets），所以"包里有什么"与
-- "exe 旁边有什么"是同一份定义，不会各自漂移。版本来自 set_version（CI 用
-- --app_version 覆盖），zip 名里的 v 只是命名习惯，版本资源里不带 v。
--
-- 平铺到包根（set_bindir(".")）：目标机上是"解压即用"的一套组合，不套 bin/ 层。
-- ============================================================================
xpack("cli")
    set_formats("zip")
    set_bindir(".")
    set_basename("IceMelt-v$(version)-cli")
    add_targets("icemelt")

xpack("gui")
    set_formats("zip")
    set_bindir(".")
    set_basename("IceMelt-GUI-v$(version)-gui")
    add_targets("icemelt-gui")
