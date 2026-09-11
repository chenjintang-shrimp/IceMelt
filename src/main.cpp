// SecMelt —— TUI 前端骨架
//
// 首屏只做一件事：前置条件自检。每一条结论都是现场实测，不预置任何假数据。
//   * 进程是否以管理员身份运行（加载内核驱动、写裸盘的前提）
//   * 系统位数与版本
//   * 随仓库携带的三方资产是否就位（KDU / WinDisk / ntfs-3g）
//
// 用法：
//   secmelt           启动交互式界面
//   secmelt --dump    渲染一帧到标准输出后退出（无 TTY 环境下的自检/回归用）
//
// 注：界面文案刻意保持 ASCII —— Windows 控制台默认代码页多为 936(GBK)，
// 而 FTXUI 走 UTF-8 输出通道，非 ASCII 文案在部分终端会花屏。源码注释不受影响。

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <windows.h>

#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ftxui;

namespace {

struct Check {
    std::string label;
    bool ok = false;
    std::string detail;
};

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring RegReadString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    if (::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, buf, &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buf, bytes / sizeof(wchar_t) - 1);
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &returned);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// 从可执行文件所在目录向上找带 third_party/ 的项目根；找不到则回退到当前目录。
fs::path FindProjectRoot() {
    std::error_code ec;
    wchar_t exe[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        fs::path dir = fs::path(exe).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (fs::is_directory(dir / "third_party", ec))
                return dir;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
    return fs::current_path(ec);
}

std::vector<Check> RunEnvironmentChecks() {
    std::vector<Check> out;

    const bool elevated = IsProcessElevated();
    out.push_back({"Administrator token", elevated,
                   elevated ? "elevated" : "NOT elevated (driver load / raw disk I/O will fail)"});

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
        if (product.rfind(kWin10, 0) == 0)
            product = L"Windows 11" + product.substr(kWin10.size());
    }

    std::string version = Narrow(product);
    if (!build.empty()) {
        version += "  build " + Narrow(build);
        if (!ubr.empty()) version += "." + Narrow(ubr);
    }
    out.push_back({"Windows version", !product.empty(), version.empty() ? "unknown" : version});

    return out;
}

std::vector<Check> RunAssetChecks(const fs::path& root) {
    struct Asset {
        const char* label;
        const char* relative;
    };
    // 路径都在仓库里真实存在；这里只做存在性判断，不代表已接入构建。
    static constexpr Asset kAssets[] = {
        {"KDU (submodule)", "third_party/KDU/Source/Hamakaze/kduprov.cpp"},
        {"WinDisk driver source", "third_party/WinDisk/Main.cpp"},
        {"WinDisk IOCTL contract", "third_party/WinDisk/Public.h"},
        {"ntfs-3g (handle: patch)", "third_party/ntfs-3g/libntfs-3g/win32_io.c"},
        {"ntfs-3g-cli", "third_party/ntfs-3g/src/ntfs-3g-cli.c"},
    };

    std::error_code ec;
    std::vector<Check> out;
    for (const auto& asset : kAssets) {
        const fs::path full = root / asset.relative;
        const bool exists = fs::is_regular_file(full, ec);
        out.push_back({asset.label, exists,
                       exists ? std::string("present")
                              : (std::string("missing: ") + asset.relative)});
    }
    return out;
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return buf;
}

// 单帧渲染：环境 / 资产 / 日志（纵向全宽，避免长文本被横向截断）
Element RenderReport(const fs::path& root,
                     const std::vector<Check>& environment,
                     const std::vector<Check>& assets,
                     const std::vector<std::string>& log) {
    // 状态 + 定宽标签（filler 补白，否则下一列会紧贴文字）+ 详情
    auto section = [](const char* title, const std::vector<Check>& items) {
        Elements rows;
        for (const auto& item : items) {
            rows.push_back(hbox({
                text(item.ok ? " ok  " : " FAIL") |
                    (item.ok ? color(Color::Green) : (color(Color::Red) | bold)),
                text(" "),
                hbox({text(item.label), filler()}) | size(WIDTH, EQUAL, 30),
                text(item.detail) | dim,
            }));
        }
        return window(text(title) | bold, vbox(std::move(rows)));
    };

    Elements log_lines;
    for (const auto& line : log)
        log_lines.push_back(text(line));

    return vbox({
               text(" SecMelt ") | bold | color(Color::Cyan),
               text(" project root: " + Narrow(root.wstring())) | dim,
               section(" Environment", environment),
               section(" Vendored assets", assets),
               window(text(" Log") | bold, vbox(std::move(log_lines))),
           }) |
           border;
}

int DumpMode(const fs::path& root) {
    const auto environment = RunEnvironmentChecks();
    const auto assets = RunAssetChecks(root);

    std::vector<std::string> log = {Timestamp() + "  checks executed (dump mode)"};
    bool all_ok = true;
    for (const auto& c : environment)
        if (!c.ok) all_ok = false;
    for (const auto& c : assets)
        if (!c.ok) all_ok = false;
    log.push_back(Timestamp() + (all_ok ? "  all prerequisites satisfied" : "  some prerequisites missing"));

    auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(24));
    Render(screen, RenderReport(root, environment, assets, log));
    std::cout << screen.ToString() << "\n";
    return all_ok ? 0 : 1;
}

int InteractiveMode(const fs::path& root) {
    auto environment = RunEnvironmentChecks();
    auto assets = RunAssetChecks(root);
    std::vector<std::string> log = {
        Timestamp() + "  checks executed",
        Timestamp() + "  use Refresh to re-run",
    };

    auto screen = ScreenInteractive::Fullscreen();

    auto refresh = Button(" Refresh ", [&] {
        environment = RunEnvironmentChecks();
        assets = RunAssetChecks(root);
        log.push_back(Timestamp() + "  checks re-executed");
        screen.PostEvent(Event::Custom);
    });
    auto quit = Button(" Quit ", screen.ExitLoopClosure());

    auto buttons = Container::Horizontal({refresh, quit});
    auto renderer = Renderer(buttons, [&] {
        return vbox({
            RenderReport(root, environment, assets, log),
            separator(),
            hbox({buttons->Render(), filler()}),
        });
    });

    screen.Loop(renderer);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--dump")
            return DumpMode(FindProjectRoot());
    }
    return InteractiveMode(FindProjectRoot());
}
