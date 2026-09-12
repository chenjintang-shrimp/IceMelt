// SecMelt —— TUI 前端
//
// 三屏：Environment / Vendored assets / Melt。
//   * Environment / Assets：前置条件自检。每一条结论都是现场实测，不预置任何假数据。
//   * Melt：名单 + 实测存在状态、运行日志、Dry run / Melt / Quit。
//
// 用法：
//   secmelt                启动交互式界面
//   secmelt --dump         渲染一帧到标准输出后退出（无 TTY 环境下的自检/回归用）
//   secmelt --dry-run      只读预演整条链路：预检 + 只读注册表探测 + 导出/校验 hive
//   secmelt --no-ntfsfix   与 --dry-run 组合保留（apply 路径不使用 ntfsfix 时需要）
//   secmelt --melt --yes-i-know 非交互执行整条链路（不需要 VT 终端；破坏性）
//   secmelt --selftest-hive 校验 hive base block 偏移与校验和算法
//   secmelt --selftest-registry 在 scratch 键上验证过滤器摘除的写入路径
//   secmelt --selftest-raw  裸盘写入/读回判定（会装载内核驱动、写系统卷，仅限虚拟机）
//
// 注：界面文案刻意保持 ASCII —— Windows 控制台默认代码页多为 936(GBK)，
// 而 FTXUI 走 UTF-8 输出通道，非 ASCII 文案在部分终端会花屏。源码注释不受影响。

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "melt/unfreeze.h"
#include "raw/win_disk.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "selftest.h"
#include "util.h"

namespace fs = std::filesystem;
using namespace ftxui;

namespace {

struct Check {
    std::string label;
    bool ok = false;
    std::string detail;
    // 失败时给出的下一步动作。A320 的 ECAM 风格：故障不能只报状态，还要告诉人该干什么。
    std::string hint;
};

// 环境自检的结果。两个"结论"单独给字段：界面需要它们做判断，而靠 checks[] 的
// 下标或 label 文案去取会在改文案时静默失效。
struct Environment {
    std::vector<Check> checks;
    bool elevated = false;
    bool dseOff = false;
};

std::wstring RegReadString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    if (::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, buf, &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buf, bytes / sizeof(wchar_t) - 1);
}

// 从可执行文件所在目录向上找带 third_party/ 的项目根；找不到则回退到当前目录。
fs::path FindProjectRoot() {
    std::error_code ec;
    wchar_t exe[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        fs::path dir = fs::path(exe).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (secmelt::PathIsDirectory(dir / "third_party")) return dir;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
    return secmelt::CurrentDirectory();
}

Environment RunEnvironmentChecks() {
    Environment env;
    std::vector<Check>& out = env.checks;

    const bool elevated = secmelt::IsProcessElevated();
    env.elevated = elevated;
    out.push_back({"Administrator token", elevated,
                   elevated ? "elevated" : "NOT elevated (driver load / raw disk I/O will fail)",
                   elevated ? "" : "relaunch from an elevated prompt (Run as administrator)"});

    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    const bool x64 = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
    out.push_back({"64-bit OS", x64, x64 ? "x64" : "non-x64"});

    constexpr wchar_t kCurrentVersion[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"ProductName");
    const std::wstring build = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"CurrentBuildNumber");
    const std::wstring ubr = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"UBR");

    // 注册表的 ProductName 自 Win10 起就再没更新过，在 Win11 上仍写着 "Windows 10"
    // （build 22000+ 才是 Win11）。按 build 号纠正，避免报出错误的产品名。
    unsigned long build_number = 0;
    if (!build.empty()) {
        try {
            build_number = std::stoul(build);
        } catch (const std::exception&) {
            build_number = 0;
        }
    }
    if (build_number >= 22000) {
        constexpr std::wstring_view kWin10 = L"Windows 10";
        if (product.rfind(kWin10, 0) == 0) product = L"Windows 11" + product.substr(kWin10.size());
    }

    std::string version = secmelt::Narrow(product);
    if (!build.empty()) {
        version += "  build " + secmelt::Narrow(build);
        if (!ubr.empty()) version += "." + secmelt::Narrow(ubr);
    }
    out.push_back({"Windows version", !product.empty(), version.empty() ? "unknown" : version,
                   "cannot read HKLM\\...\\Windows NT\\CurrentVersion"});

    bool dseOff = false;
    std::wstring dseError;
    if (secmelt::DseDisabled(dseOff, dseError)) {
        env.dseOff = dseOff;
        // DSE 开着**不算故障**：apply 路径会自己用 KDU 关掉它（见 DisableDseWithKdu）。
        // 所以这里只报告状态、不标红 —— 冷舱原则：只有真的需要人处理的问题才醒目。
        out.push_back({"Driver signature enforcement", true,
                       dseOff ? "disabled" : "enabled (melt turns it off via kdu)", ""});
    } else {
        // 查不到就是真的不确定，仍然标红
        out.push_back({"Driver signature enforcement", false, secmelt::Narrow(dseError),
                       "run: kdu -diag"});
    }

    return env;
}

std::vector<Check> RunAssetChecks(const fs::path& root) {
    struct Asset {
        const char* label;
        const char* relative;
    };
    static constexpr Asset kAssets[] = {
        {"KDU (submodule)", "third_party/KDU/Source/Hamakaze/kduprov.cpp"},
        {"WinDisk driver source", "third_party/WinDisk/Main.cpp"},
        {"WinDisk IOCTL contract", "third_party/WinDisk/Public.h"},
        {"ntfs-3g (handle: patch)", "third_party/ntfs-3g/libntfs-3g/win32_io.c"},
        {"ntfs-3g-cli", "third_party/ntfs-3g/src/ntfs-3g-cli.c"},
        {"target list", "config/targets.txt"},
    };

    std::vector<Check> out;
    for (const auto& asset : kAssets) {
        const fs::path full = root / asset.relative;
        const bool exists = secmelt::PathIsRegularFile(full);
        out.push_back({asset.label, exists,
                       exists ? std::string("present")
                              : (std::string("missing: ") + asset.relative),
                       exists ? "" : "run: xmake build (or check the submodule)"});
    }
    return out;
}

// 名单里每一项在本机的实测存在状态
struct TargetRow {
    std::string name;
    std::string label;
    std::string status;
    bool present = false;
};

std::vector<TargetRow> ProbeTargetRows(const fs::path& exeDir) {
    const auto targets = secmelt::LoadTargets(exeDir);
    const auto names = secmelt::TargetNames(targets);
    const secmelt::ProbeReport probe = secmelt::ProbeTargets(names);

    std::vector<TargetRow> rows;
    rows.reserve(targets.size());
    for (const auto& t : targets) {
        TargetRow row;
        row.name = secmelt::Narrow(t.name);
        row.label = secmelt::Narrow(t.label);

        std::vector<std::string> where;
        for (const auto& f : probe.filters) {
            for (const auto& item : f.items) {
                if (_wcsicmp(item.c_str(), t.name.c_str()) == 0) {
                    where.push_back("Class\\" + secmelt::Narrow(f.classGuid) + "\\" +
                                    secmelt::Narrow(f.valueName));
                }
            }
        }
        for (const auto& key : probe.existingServiceKeys) {
            if (_wcsicmp(key.c_str(), t.name.c_str()) == 0)
                where.push_back("Services\\" + secmelt::Narrow(key));
        }

        row.present = !where.empty();
        if (where.empty()) {
            row.status = "not present";
        } else {
            row.status = "PRESENT: " + where[0];
            for (size_t i = 1; i < where.size(); ++i) row.status += ", " + where[i];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return buf;
}

// ---- 渲染 -------------------------------------------------------------------
//
// A320 "cold & dark" 原则：一切正常时几乎不显示任何东西，只有异常才醒目地跳出来，
// 并且必须带上"下一步做什么"。这里的直接收益有两个：
//   * 人一眼就能看出有没有问题（正常时没有一屏的 "ok" 要去逐行扫）；
//   * 每帧要重绘的单元格少一个数量级 —— 这对 ConEmu 这类逐行绘制、没有双缓冲的
//     终端很关键：整屏重写的行数越少，重绘时的可见撕裂越小。
//
// 布局上刻意只有最外层一圈边框：内层不再套 window()。嵌套的边框除了好看没有任何
// 信息量，却要重画大量单元格。

// 未通过的检查项（顺序保持，便于稳定复现）
std::vector<const Check*> Faults(const std::vector<Check>& a, const std::vector<Check>& b) {
    std::vector<const Check*> out;
    for (const auto* list : {&a, &b}) {
        for (const auto& c : *list) {
            if (!c.ok) out.push_back(&c);
        }
    }
    return out;
}

// 正常时的状态摘要（一行，暗色）：具体数值只在"可核对"的意义上有用，
// 不需要占一整屏。
Element NominalLine(size_t assetOk, size_t assetTotal, const Environment& environment) {
    std::string detail = std::string(environment.elevated ? "admin" : "no admin");
    detail += environment.dseOff ? " · DSE off" : " · DSE ON";
    detail += " · assets " + std::to_string(assetOk) + "/" + std::to_string(assetTotal);
    return hbox({
        text(" ALL SYSTEMS GO ") | bold | color(Color::Green),
        text("  " + detail + " ") | dim,
        filler(),
    });
}

// 异常时：标题 + 逐条故障 + 去重后的处置动作
Element FaultBlock(const std::vector<const Check*>& faults) {
    Elements rows;
    rows.push_back(hbox({
        text(" ") ,
        text(std::to_string(faults.size()) + " CHECK" + (faults.size() == 1 ? "" : "S") + " FAILED") |
            bold | color(Color::Red),
        filler(),
    }));
    for (const auto* f : faults) {
        // 用显式间隔而不是定宽列：定宽列在标签长度等于列宽时会挤掉间隙
        // （实测 "Driver signature enforcement" 正好 28 字符 → "enforcementENABLED"）。
        rows.push_back(hbox({
            text("  x ") | bold | color(Color::Red),
            text(f->label + "  ") | color(Color::Red),
            text(f->detail) | color(Color::Red),
            filler(),
        }));
    }

    // 处置动作去重：多个故障可能指向同一个动作
    std::vector<std::string> hints;
    for (const auto* f : faults) {
        if (f->hint.empty()) continue;
        if (std::find(hints.begin(), hints.end(), f->hint) == hints.end()) hints.push_back(f->hint);
    }
    for (const auto& hint : hints) {
        rows.push_back(hbox({
            text("  -> ") | bold,
            text(hint) | bold,
            filler(),
        }));
    }
    return vbox(std::move(rows));
}

Element RenderEnvironment(const Environment& environment, const std::vector<Check>& assets) {
    const auto faults = Faults(environment.checks, assets);
    size_t assetOk = 0;
    for (const auto& a : assets) {
        if (a.ok) ++assetOk;
    }

    Elements rows;
    rows.push_back(separator());
    if (faults.empty()) {
        rows.push_back(NominalLine(assetOk, assets.size(), environment));
    } else {
        rows.push_back(FaultBlock(faults));
    }
    return vbox(std::move(rows));
}

// 每个屏幕的标题行：标题在左、项目根在右（项目根只在需要时看一眼，所以压暗）。
// 界面顶部已经有 Tab 标签，标题行主要是给 --dump 的两帧做区分的。
Element ScreenHeader(const char* title, const fs::path* root) {
    Elements row = {text(title ? title : " SecMelt") | bold | color(Color::Cyan)};
    if (root) {
        row.push_back(filler());
        row.push_back(text(secmelt::Narrow(root->wstring()) + " ") | dim);
    } else {
        row.push_back(filler());
    }
    return hbox(std::move(row));
}

// Environment 屏的完整一帧（--dump 与交互界面共用，避免两处布局各自漂移）
Element EnvironmentScreen(const char* title, const fs::path& root, const Environment& environment,
                          const std::vector<Check>& assets, const std::vector<std::string>& log) {
    Elements rows = {ScreenHeader(title, &root), RenderEnvironment(environment, assets)};
    if (!log.empty()) {
        rows.push_back(separator());
        for (const auto& line : log) rows.push_back(text(line) | dim);
    }
    return vbox(std::move(rows));
}

Element RenderMelt(const std::vector<TargetRow>& rows, const Environment& environment) {
    Elements list;
    list.push_back(hbox({
        text("   ") ,
        hbox({text("name"), filler()}) | size(WIDTH, EQUAL, 16),
        text("target") | size(WIDTH, EQUAL, 28),
        text("state on this machine") | dim,
    }));
    for (const auto& row : rows) {
        list.push_back(hbox({
            text(row.present ? " HIT " : "  -  ") |
                (row.present ? (color(Color::Red) | bold) : color(Color::GrayDark)),
            text(" "),
            hbox({text(row.name), filler()}) | size(WIDTH, EQUAL, 16),
            text(row.label) | size(WIDTH, EQUAL, 28),
            text(row.status) | dim,
        }));
    }
    // 就绪只看管理员：DSE 由 melt 自己关（kdu -dse 0），不需要人先处理。
    const bool ready = environment.elevated;
    list.push_back(hbox({
        text("  "),
        text(ready ? "ready to melt" : "NOT ready") |
            (ready ? color(Color::Green) : (bold | color(Color::Red))),
        text(ready ? (environment.dseOff ? "  (admin; DSE already off)"
                                         : "  (admin; DSE will be turned off by kdu)")
                   : "  (needs an elevated prompt)") | dim,
        filler(),
    }));
    return vbox(std::move(list));
}

// Melt 屏的完整一帧
Element MeltScreen(const char* title, const std::vector<TargetRow>& rows,
                   const Environment& environment, Element extra,
                   const std::vector<std::string>& log) {
    Elements body = {ScreenHeader(title, nullptr), RenderMelt(rows, environment)};
    if (extra) body.push_back(std::move(extra));
    body.push_back(separator());
    for (const auto& line : log) body.push_back(text(line));
    return vbox(std::move(body));
}

// Screen::ToString() 会把样式写成 SGR 转义序列（\x1b[1m 之类）——在日志/CI 里是噪音，
// 在没有 VT 支持的控制台（Windows 7 的 conhost）上更会直接花屏。--dump 要的是可读、
// 可 diff 的文本，所以把这些序列剥掉。
//
// （不用 Screen::at() 逐格拼：它对空白单元格返回空串，拼出来的框线会缺格。）
std::string StripAnsi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            // CSI 的参数字节是 0x30-0x3F，中间字节是 0x20-0x2F，终止字节是 0x40-0x7E
            while (i < in.size() && !(in[i] >= 0x40 && in[i] <= 0x7E)) ++i;
            if (i < in.size()) ++i;
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

std::string PlainScreen(const Screen& screen) {
    std::vector<std::string> lines;
    std::istringstream input(StripAnsi(screen.ToString()));
    for (std::string line; std::getline(input, line);) {
        while (!line.empty() && line.back() == ' ') line.pop_back();  // 去掉行尾填充
        lines.push_back(std::move(line));
    }
    // 去掉尾部空行：--dump 要反映实际画了些什么，而不是屏幕网格的剩余部分
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out.push_back('\n');
    }
    return out;
}

// 读一个 ANSI 环境变量。用 Win32 的 GetEnvironmentVariableA 而不是 CRT 的 getenv：
// getenv 在 MSVC 下被标记为 deprecated（会报 C4996），而这里只需要读几个短字符串，
// 栈缓冲足够、也不必分配。
bool EnvVarEquals(const char* name, const char* expected) {
    char buf[64] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return n > 0 && n < sizeof(buf) && _stricmp(buf, expected) == 0;
}

bool EnvVarSet(const char* name) {
    char buf[8] = {};
    return ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf))) > 0;
}

// 当前控制台能否解释 VT 转义序列。FTXUI 的交互界面完全依赖它们：
//   * Windows 10 1511 起 conhost 支持 ENABLE_VIRTUAL_TERMINAL_PROCESSING(0x0004)；
//     Windows 7/8 的 conhost 不认识这个标志，SetConsoleMode 会返回
//     ERROR_INVALID_PARAMETER，转义序列被原样打印出来。
//   * ConEmu / ANSICON 自己解析转义序列（ConEmu 还会监视这个标志），
//     因此它们注入的环境变量同样算"支持"。
// 两者都不成立时启动交互界面只会刷屏，调用方据此给出可操作的提示。
bool ConsoleSupportsVt(std::string& how) {
    if (EnvVarEquals("ConEmuANSI", "ON")) {
        how = "ConEmu";
        return true;
    }
    if (EnvVarSet("ANSICON")) {
        how = "ANSICON";
        return true;
    }
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && ::GetConsoleMode(out, &mode)) {
        if (::SetConsoleMode(out, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */)) {
            ::SetConsoleMode(out, mode);  // 探测完就还原，交给 FTXUI 自己设置
            how = "conhost/VT";
            return true;
        }
    }
    return false;
}

int DumpMode(const fs::path& root) {
    const auto environment = RunEnvironmentChecks();
    const auto assets = RunAssetChecks(root);

    std::vector<std::string> log = {Timestamp() + "  checks executed (dump mode)"};
    bool all_ok = true;
    for (const auto& c : environment.checks)
        if (!c.ok) all_ok = false;
    for (const auto& c : assets)
        if (!c.ok) all_ok = false;
    log.push_back(Timestamp() +
                  (all_ok ? "  all prerequisites satisfied" : "  some prerequisites missing"));

    auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(28));
    Render(screen, EnvironmentScreen(" SecMelt", root, environment, assets, log));
    std::cout << PlainScreen(screen) << "\n";
    // 第二帧：Melt 屏（名单的实测存在状态 + 运行日志窗口），同样是现场探测结果
    auto meltScreen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(28));
    const std::vector<std::string> meltPreview = {
        Timestamp() + "  checks executed (dump mode)",
        Timestamp() + "  Dry run rehearses the whole chain and writes nothing",
    };
    Render(meltScreen, MeltScreen(" SecMelt :: Melt", ProbeTargetRows(root), environment, nullptr,
                                  meltPreview));
    std::cout << PlainScreen(meltScreen) << "\n";
    return all_ok ? 0 : 1;
}

int InteractiveMode(const fs::path& root) {
    auto environment = RunEnvironmentChecks();
    auto assets = RunAssetChecks(root);
    auto rows = ProbeTargetRows(root);

    // 运行中的 Melt 在后台线程里跑：破坏性操作期间界面必须还能刷新日志
    std::mutex meltMutex;
    std::vector<std::string> meltLog = {
        Timestamp() + "  ready",
        Timestamp() + "  Dry run: rehearse the whole chain, write nothing",
        Timestamp() + "  Melt: strip filters, write the hive back, then bugcheck-reset",
    };
    // Melt 状态：三个独立的 atomic 会互相打架（例如"刚置 running 又被 finished 覆盖"），
    // 用一个在互斥锁下的枚举就够，而且渲染时能和日志取到同一个快照。
    enum class MeltState { Idle, Running, FinishedOk, FinishedFailed };
    MeltState meltState = MeltState::Idle;
    bool startRequested = false;

    // ntfsfix 是否参与落盘（对应 CLI 的 --no-ntfsfix）；必须在 startMelt 之前声明
    bool runNtfsFix = true;

    // TerminalOutput 而不是 Fullscreen：
    //   * 高度跟着内容走 —— 每帧只重绘实际有内容的那些行。Fullscreen 会把网格撑到
    //     整个终端高度（VM 里几十行）并切到备用屏幕（?1049）。ConEmu 是逐行绘制、
    //     没有整帧合成（它还会因为"Max real console size"受限），重绘面积越大越容易
    //     看到撕裂；Windows Terminal 有更好的合成所以不明显。
    //   * 宽度仍等于终端宽度 —— 这一点必须保留：FitComponent 会用"组件的自然宽度"
    //     当屏幕宽度，那样所有 filler() 右对齐都会塌陷（实测标题与路径会粘成
    //     "SecMeltD:\sectl\SecMelt"）。
    auto screen = ScreenInteractive::TerminalOutput();

    const auto appendLocked = [&](const std::string& line) {
        meltLog.push_back(Timestamp() + "  " + line);
    };
    const auto appendWide = [&](const std::wstring& line) {
        std::lock_guard<std::mutex> guard(meltMutex);
        appendLocked(secmelt::Narrow(line));
    };

    const auto startMelt = [&](bool dryRun) {
        {
            std::lock_guard<std::mutex> guard(meltMutex);
            if (meltState == MeltState::Running) return;
            meltState = MeltState::Running;
            appendLocked(dryRun ? "dry run requested" : "melt requested");
        }
        startRequested = true;

        std::thread([&, dryRun] {
            secmelt::MeltOptions opt;
            opt.dryRun = dryRun;
            opt.runNtfsFix = runNtfsFix;
            opt.exeDir = root;
            opt.winDiskSysPath = secmelt::ExeDir() / L"WinDisk_x64.sys";
            if (!dryRun) {
                // 确认对话框走原生 MessageBox：RunMelt 是同步调用，而 FTXUI 的事件循环
                // 不能从处理器内部再嵌一层循环。
                opt.confirm = [&](const secmelt::MeltResult& pending) {
                    std::wstring text =
                        L"About to write the SYSTEM hive to the raw disk, then reset the machine\n"
                        L"by bugchecking with 0x0D000721 (CTL_REBOOT_SYSTEM).\n\n"
                        L"The following actions are irreversible:\n";
                    for (const auto& item : pending.pending) text += L"  - " + item + L"\n";
                    text += L"\nContinue?";
                    return ::MessageBoxW(nullptr, text.c_str(), L"SecMelt",
                                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
                };
            }
            const secmelt::MeltResult result = secmelt::RunMelt(opt, appendWide);
            {
                std::lock_guard<std::mutex> guard(meltMutex);
                meltState = result.ok ? MeltState::FinishedOk : MeltState::FinishedFailed;
            }
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    const auto doRefresh = [&] {
        environment = RunEnvironmentChecks();
        assets = RunAssetChecks(root);
        rows = ProbeTargetRows(root);
        {
            std::lock_guard<std::mutex> guard(meltMutex);
            appendLocked("checks re-executed");
        }
        screen.PostEvent(Event::Custom);
    };
    auto refresh = Button(" Refresh ", doRefresh);
    auto dryRunButton = Button(" Dry run ", [&] { startMelt(true); });
    auto meltButton = Button(" Melt ", [&] { startMelt(false); });
    auto quit = Button(" Quit ", screen.ExitLoopClosure());

    auto ntfsfixBox = Checkbox(" run ntfsfix before ntfscp", &runNtfsFix);

    int tab = 0;
    std::vector<std::string> tabTitles = {"Environment", "Melt"};
    auto tabToggle = Toggle(&tabTitles, &tab);

    auto buttons = Container::Horizontal({refresh, dryRunButton, meltButton, quit});
    // 焦点链：标签切换 → Melt 页上的 ntfsfix 勾选 → 底部按钮。
    // Toggle / Checkbox 不在组件树里就永远拿不到键盘焦点（只能看不能用）；
    // 勾选框只在 Melt 页参与焦点链（Container::Tab 会用页面索引覆盖 Toggle 的选择，
    // 两者共用同一个变量会互相打架，故这里只用 Maybe 做条件挂载）。
    auto content =
        Container::Vertical({tabToggle, Maybe(ntfsfixBox, [&] { return tab == 1; }), buttons});
    auto renderer = Renderer(content, [&] {
        std::vector<std::string> snapshot;
        MeltState snapshotState = MeltState::Idle;
        {
            std::lock_guard<std::mutex> guard(meltMutex);
            snapshot = meltLog;
            snapshotState = meltState;
        }
        std::string state = "idle";
        switch (snapshotState) {
            case MeltState::Running: state = "running..."; break;
            case MeltState::FinishedOk: state = "finished OK"; break;
            case MeltState::FinishedFailed: state = "finished with errors"; break;
            case MeltState::Idle: break;
        }

        Element body = (tab == 0)
                           ? EnvironmentScreen(" SecMelt", root, environment, assets, {})
                           : MeltScreen(" SecMelt :: Melt", rows, environment, ntfsfixBox->Render(),
                                        snapshot);
        return vbox({
                   hbox({text(" SecMelt ") | bold | color(Color::Cyan), filler(), tabToggle->Render()}),
                   separator(),
                   std::move(body),
                   separator(),
                   hbox({buttons->Render(), filler(),
                         text(" keys: 1/2 tab  d dry run  m melt  r refresh  q quit ") | dim,
                         text(" state: " + state + " ") | dim}),
               }) |
               border;
    });

    // 按键直通：按钮同时支持鼠标点击与 Tab+Enter，但终端不保证把 Tab 送进来，
    // 这里再给一组字母快捷键，保证纯键盘也能走完整条链路。
    auto app = CatchEvent(renderer, [&](Event event) {
        if (!event.is_character()) return false;
        const std::string ch = event.character();
        if (ch == "1") { tab = 0; return true; }
        if (ch == "2") { tab = 1; return true; }
        if (ch == "d") { startMelt(true); return true; }
        if (ch == "m") { startMelt(false); return true; }
        if (ch == "r") { doRefresh(); return true; }
        if (ch == "q") { screen.ExitLoopClosure()(); return true; }
        return false;
    });

    screen.Loop(app);
    // 线程是 detach 的，但它持有对环境/屏幕的引用；等它跑完再离开作用域才是安全的。
    if (startRequested) {
        for (;;) {
            {
                std::lock_guard<std::mutex> guard(meltMutex);
                if (meltState != MeltState::Running) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return 0;
}

void PrintUsage() {
    std::cout << "SecMelt\n"
                 "  secmelt                  interactive TUI\n"
                 "  secmelt --dump           render one frame and exit (no TTY self-check)\n"
                 "  secmelt --dry-run        rehearse the whole chain without writing the disk\n"
                 "  secmelt --no-ntfsfix     skip the ntfsfix step (works with --dry-run/--melt)\n"
                 "  secmelt --melt --yes-i-know\n"
                 "                           non-interactive apply: strip filters, write the hive,\n"
                 "                           zero the logs, then reset by bugcheck 0x0D000721\n"
                 "  secmelt --selftest-hive  verify regf base block offsets and checksum algorithm\n"
                 "  secmelt --selftest-registry\n"
                 "                           exercise the filter-stripping write path on a scratch key\n"
                 "  secmelt --selftest-raw   raw disk write/read-back check (loads a kernel driver,\n"
                 "                           writes the system volume: run in a VM only)\n";
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    bool dryRun = false;
    bool noNtfsFix = false;
    bool selftestHive = false;
    bool selftestRegistry = false;
    bool selftestRaw = false;
    bool dump = false;
    bool help = false;
    bool melt = false;
    bool yesIKnow = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dump") dump = true;
        else if (arg == "--dry-run") dryRun = true;
        else if (arg == "--no-ntfsfix") noNtfsFix = true;
        else if (arg == "--melt") melt = true;
        else if (arg == "--yes-i-know") yesIKnow = true;
        else if (arg == "--selftest-hive") selftestHive = true;
        else if (arg == "--selftest-registry") selftestRegistry = true;
        else if (arg == "--selftest-raw") selftestRaw = true;
        else if (arg == "--help" || arg == "-h") help = true;
        else {
            std::cout << "unknown option: " << arg << "\n";
            PrintUsage();
            return 2;
        }
    }

    const fs::path root = FindProjectRoot();
    const fs::path exeDir = secmelt::ExeDir();

    if (help) {
        PrintUsage();
        return 0;
    }
    if (melt) {
        // 落盘不可逆：没有显式 token 就不执行（这也是唯一的非交互入口，
        // 因此 TUI 那条路的确认对话框仍然保留）。
        if (!yesIKnow) {
            std::cout << "--melt writes the SYSTEM hive to the raw disk, zeroes SYSTEM.LOG1/LOG2\n"
                         "and resets the machine by bugcheck 0x0D000721. There is no rollback.\n"
                         "Re-run with --melt --yes-i-know if that is what you want.\n";
            return 2;
        }
        return secmelt::MeltApply(exeDir, !noNtfsFix);
    }
    if (selftestHive) return secmelt::HiveSelfTest(exeDir);
    if (selftestRegistry) return secmelt::RegistrySelfTest(exeDir);
    if (selftestRaw) return secmelt::RawSelfTest(exeDir);
    if (dryRun) return secmelt::MeltDryRun(exeDir.empty() ? root : exeDir, !noNtfsFix);
    if (dump) return DumpMode(root);

    // 交互界面完全靠 VT 转义序列渲染。Windows 7/8 的 conhost 不认识它们，会把
    // `\x1b[1m` 之类原样打出来 —— 那比直接拒绝更让人困惑，所以这里先探测再决定。
    // 无 VT 时可用的等效路径：ConEmu/ANSICON，或纯文本的
    // --dry-run / --melt --yes-i-know / --selftest-*。
    std::string vtVia;
    if (!ConsoleSupportsVt(vtVia) && !EnvVarSet("SECMELT_TUI_FORCE")) {
        std::cout << "This console cannot interpret VT escape sequences, so the interactive UI\n"
                     "would print raw escape codes instead of drawing.\n\n"
                     "Options:\n"
                     "  * run secmelt inside ConEmu (or ANSICON), which parse them themselves\n"
                     "  * use the plain-text commands instead:\n"
                     "      secmelt --dump                     one-frame self-check\n"
                     "      secmelt --dry-run                  rehearse the whole chain\n"
                     "      secmelt --melt --yes-i-know        execute the chain (destructive)\n"
                     "  * set SECMELT_TUI_FORCE=1 to start the UI anyway\n";
        return 3;
    }
    return InteractiveMode(root);
}
