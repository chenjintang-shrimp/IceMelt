// SecMelt —— 图形前端（Dear ImGui · Win32 + Direct3D9）
//
// 设计取舍：
//   * 与 TUI（src/main.cpp）共用同一条 pipeline（melt/unfreeze.h），不包壳子进程；
//   * 日志经 MeltLogger 回调流入环形缓冲（worker 线程 push、UI 线程 drain）；
//   * 落盘确认经 MeltOptions::confirm 回调挂到 ImGui 模态框（condvar 等回答）；
//   * 三个动作与 CLI 一一对应：预检扫描 = --preflight、只读预演 = --dry-run、
//     执行 MELT = --melt --yes-i-know（键入 MELT 就是那张 --yes-i-know）。
//
// 平台约束：Windows 7 SP1 → Windows 11。后端选 D3D9：Vista 起随系统提供，
// 无可再发行组件。本文件里禁止 std::filesystem 的"打开句柄"系调用 —— MSVC/clang-cl
// 的 STL 会把它们链到 CreateFile2(Win8+)，Win7 上启动即失败；文件/目录谓词一律走
// util.h 的 PathIs* / RemoveFile。
//
// 安全边界：GUI 启动本身只做只读探测（注册表名单 + 资产清点），不装载驱动、不碰盘；
// 驱动装载、裸盘写入只发生在用户显式点击之后。非提权时三个动作按钮全部禁用。

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <condition_variable>
#include <cfloat>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "melt/unfreeze.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

namespace fs = std::filesystem;

// ---- 任务模型 ---------------------------------------------------------------

enum class JobKind { None, Preflight, DryRun, Melt };

const char* JobName(JobKind kind) {
    switch (kind) {
        case JobKind::Preflight: return "预检扫描";
        case JobKind::DryRun: return "只读预演";
        case JobKind::Melt: return "执行 MELT";
        default: return "空闲";
    }
}

// worker 线程与 UI 线程之间唯一的共享状态。所有字段都在 mu 下访问；
// 条件变量只用于 confirm 的一问一答。
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> lines;  // 日志行（UTF-8），封顶 4000
    size_t totalLines = 0;          // 单调递增，UI 侧据此追赶
    bool confirmRequested = false;
    bool confirmAnswered = false;
    bool confirmAnswer = false;
    std::vector<std::string> pending;  // 待确认清单（第 7 步）
    bool done = false;
    secmelt::MeltResult result;
};

struct AssetRow {
    const char* label;
    const wchar_t* rel;
    bool present = false;
};

struct TargetRowUI {
    std::string name;
    std::string label;
    std::string status;
    bool present = false;
};

// 名单文件的观测戳：用于"文件一变就重载"（大小 + 修改时间；不存在时 valid=false）
struct FileStamp {
    bool valid = false;
    ULONGLONG size = 0;
    ULONGLONG mtime = 0;
    bool operator!=(const FileStamp& other) const {
        return valid != other.valid || size != other.size || mtime != other.mtime;
    }
};

struct App {
    HWND hwnd = nullptr;
    float dpiScale = 1.0f;

    bool elevated = false;
    std::string osVersion;
    std::string exeDirUtf8;
    std::vector<AssetRow> assets;
    std::vector<TargetRowUI> targets;

    // 名单来源与变更观测（动态加载：按钮 + 文件变更自动重载）
    fs::path targetsFile;        // 空 = 未找到 targets.txt，用的是内置默认名单
    FileStamp targetsStamp;
    double nextTargetsCheck = 0.0;

    bool runNtfsFix = true;

    JobKind kind = JobKind::None;
    JobKind lastKind = JobKind::None;
    bool running = false;
    bool lastOk = false;
    bool lastWroteDisk = false;

    // confirm 模态框
    bool confirmOpen = false;
    std::vector<std::string> confirmItems;
    char confirmBuf[32] = {};

    bool rebootModal = false;
    std::string rebootError;

    // 日志视图（UI 线程私有）
    std::vector<std::string> lines;
    size_t seen = 0;
    bool follow = true;

    std::thread worker;
    Shared shared;
};

App* g_app = nullptr;

// ---- 只读探测（与 src/main.cpp 保持一致）-------------------------------------

void UiLog(App& app, const std::string& line);  // 定义在"日志管线"一节

std::wstring RegReadString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    if (::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, buf, &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buf, bytes / sizeof(wchar_t) - 1);
}

std::string WindowsVersionString() {
    constexpr wchar_t kCurrentVersion[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"ProductName");
    const std::wstring build = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"CurrentBuildNumber");
    const std::wstring ubr = RegReadString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"UBR");

    unsigned long buildNumber = 0;
    if (!build.empty()) {
        try {
            buildNumber = std::stoul(build);
        } catch (...) {
            buildNumber = 0;
        }
    }
    // ProductName 自 Win10 起不再更新（Win11 上仍写 "Windows 10"），按 build 号纠正
    if (buildNumber >= 22000) {
        constexpr wchar_t kWin10[] = L"Windows 10";
        constexpr size_t kWin10Len = (sizeof(kWin10) / sizeof(wchar_t)) - 1;
        if (product.compare(0, kWin10Len, kWin10) == 0)
            product = std::wstring(L"Windows 11") + product.substr(kWin10Len);
    }

    std::string version = secmelt::Narrow(product);
    if (!build.empty()) {
        version += "  build " + secmelt::Narrow(build);
        if (!ubr.empty()) version += "." + secmelt::Narrow(ubr);
    }
    return version.empty() ? "unknown" : version;
}

void RefreshAssets(App& app, const fs::path& exeDir) {
    static const AssetRow kAssets[] = {
        {"WinDisk 驱动", L"WinDisk_x64.sys"},
        {"KDU（关签名强制）", L"kdu.exe"},
        {"KDU provider 库", L"drv64.dll"},
        {"目标名单", L"targets.txt"},
        {"ntfsfix", L"tools\\ntfsfix.exe"},
        {"ntfscp", L"tools\\ntfscp.exe"},
        {"ntfs-3g-cli", L"tools\\ntfs-3g-cli.exe"},
        {"mkntfs", L"tools\\mkntfs.exe"},
    };
    app.assets.clear();
    for (const auto& a : kAssets) {
        AssetRow row = a;
        row.present = secmelt::PathIsRegularFile(exeDir / a.rel);
        app.assets.push_back(row);
    }
}

// 与 LoadTargets 相同的搜索顺序（<exeDir>/targets.txt → <exeDir>/config/targets.txt）；
// 返回空表示两个都没有，实际用的是编译内置名单。
fs::path ResolveTargetsFile(const fs::path& exeDir) {
    const fs::path first = exeDir / L"targets.txt";
    if (secmelt::PathIsRegularFile(first)) return first;
    const fs::path second = exeDir / L"config" / L"targets.txt";
    if (secmelt::PathIsRegularFile(second)) return second;
    return {};
}

FileStamp StatFile(const fs::path& p) {
    FileStamp st;
    if (p.empty()) return st;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return st;
    st.valid = true;
    st.size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    st.mtime = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
               fad.ftLastWriteTime.dwLowDateTime;
    return st;
}

// 重新读名单文件并重解析。announce=true 时把来源写进日志（启动/重载/手动刷新用）。
void RefreshTargets(App& app, const fs::path& exeDir, bool announce) {
    const auto targets = secmelt::LoadTargets(exeDir);
    const auto names = secmelt::TargetNames(targets);
    const secmelt::ProbeReport probe = secmelt::ProbeTargets(names);

    app.targetsFile = ResolveTargetsFile(exeDir);
    app.targetsStamp = StatFile(app.targetsFile);

    app.targets.clear();
    app.targets.reserve(targets.size());
    for (const auto& t : targets) {
        TargetRowUI row;
        row.name = secmelt::Narrow(t.name);
        row.label = secmelt::Narrow(t.label);

        std::vector<std::string> where;
        for (const auto& f : probe.filters) {
            for (const auto& item : f.items) {
                if (_wcsicmp(item.c_str(), t.name.c_str()) == 0)
                    where.push_back("Class\\" + secmelt::Narrow(f.classGuid) + "\\" +
                                    secmelt::Narrow(f.valueName));
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
        app.targets.push_back(std::move(row));
    }

    if (announce) {
        if (app.targetsFile.empty()) {
            UiLog(app, "[!] 目标名单: 未找到 targets.txt，使用内置默认名单");
        } else {
            UiLog(app, "[*] 目标名单已加载: " + secmelt::Narrow(app.targetsFile.wstring()));
        }
    }
}

// 名单文件一变就重载（0.5s 一次的取证级轮询：大小 + mtime）。监视的是"当前生效"的
// 路径：被删/被建/被改都会让戳改变，重载后 ResolveTargetsFile 再决定用哪一个。
void TickTargetsReload(App& app, double now) {
    if (now < app.nextTargetsCheck) return;
    app.nextTargetsCheck = now + 0.5;
    const fs::path exeDir = secmelt::ExeDir();
    const FileStamp current = StatFile(ResolveTargetsFile(exeDir));
    if (current != app.targetsStamp) {
        RefreshTargets(app, exeDir, true);
    }
}

// ---- 日志管线 ---------------------------------------------------------------

void UiLog(App& app, const std::string& line) {
    app.lines.push_back(line);
    if (app.lines.size() > 5000) app.lines.erase(app.lines.begin(), app.lines.begin() + 1000);
    app.follow = true;
}

void DrainSharedLines(App& app) {
    std::lock_guard<std::mutex> guard(app.shared.mu);
    const size_t extra = app.shared.totalLines - app.seen;
    const size_t n = extra < app.shared.lines.size() ? extra : app.shared.lines.size();
    for (size_t i = app.shared.lines.size() - n; i < app.shared.lines.size(); ++i)
        app.lines.push_back(app.shared.lines[i]);
    app.seen = app.shared.totalLines;
    if (app.lines.size() > 5000) app.lines.erase(app.lines.begin(), app.lines.begin() + 1000);
}

// 与 util.h 的 CliPaint 同一分层：出错必高亮，其余保持冷色（Catppuccin Mocha）
ImU32 LineColor(const std::string& s) {
    const bool fail = s.rfind("[x]", 0) == 0 || s.find("FAIL") != std::string::npos;
    if (fail) return IM_COL32(243, 139, 168, 255);   // Mocha red
    if (s.rfind("[!]", 0) == 0) return IM_COL32(249, 226, 175, 255);  // Mocha yellow
    return IM_COL32(205, 214, 244, 255);             // Mocha text
}

// ---- 任务启停 ---------------------------------------------------------------

void RunJob(Shared& sh, fs::path exeDir, JobKind kind, bool runNtfsFix) {
    secmelt::MeltOptions opt;
    opt.exeDir = std::move(exeDir);
    opt.winDiskSysPath = opt.exeDir / L"WinDisk_x64.sys";
    opt.dryRun = (kind == JobKind::DryRun);
    opt.runNtfsFix = runNtfsFix;

    const secmelt::MeltLogger logger = [&sh](const std::wstring& w) {
        std::lock_guard<std::mutex> guard(sh.mu);
        sh.lines.push_back(secmelt::Narrow(w));
        if (sh.lines.size() > 4000) sh.lines.pop_front();
        ++sh.totalLines;
    };

    if (kind == JobKind::Melt) {
        opt.confirm = [&sh](const secmelt::MeltResult& pending) -> bool {
            std::unique_lock<std::mutex> lock(sh.mu);
            sh.pending.clear();
            for (const auto& item : pending.pending) sh.pending.push_back(secmelt::Narrow(item));
            sh.confirmAnswered = false;
            sh.confirmAnswer = false;
            sh.confirmRequested = true;
            sh.cv.notify_all();
            sh.cv.wait(lock, [&sh] { return sh.confirmAnswered; });
            const bool answer = sh.confirmAnswer;
            sh.confirmRequested = false;
            return answer;
        };
    }

    secmelt::MeltResult result;
    if (kind == JobKind::Preflight) {
        result = secmelt::RunPreflightScan(opt, logger);
    } else {
        result = secmelt::RunMelt(opt, logger);
    }

    std::lock_guard<std::mutex> guard(sh.mu);
    sh.result = std::move(result);
    sh.done = true;
}

void StartJob(App& app, JobKind kind) {
    if (app.running) return;
    app.kind = kind;
    app.running = true;
    app.lastOk = false;
    app.lastWroteDisk = false;
    app.lines.clear();
    app.seen = 0;
    app.follow = true;

    {
        std::lock_guard<std::mutex> guard(app.shared.mu);
        app.shared.lines.clear();
        app.shared.totalLines = 0;
        app.shared.confirmRequested = false;
        app.shared.confirmAnswered = false;
        app.shared.confirmAnswer = false;
        app.shared.pending.clear();
        app.shared.done = false;
        app.shared.result = secmelt::MeltResult{};
    }

    app.worker = std::thread(RunJob, std::ref(app.shared), secmelt::ExeDir(), kind, app.runNtfsFix);
}

void PollWorker(App& app) {
    if (!app.running) return;

    // 抽取日志
    DrainSharedLines(app);

    // 检查 confirm
    {
        std::lock_guard<std::mutex> guard(app.shared.mu);
        if (app.shared.confirmRequested && !app.confirmOpen) {
            app.confirmOpen = true;
            app.confirmItems = app.shared.pending;
            std::memset(app.confirmBuf, 0, sizeof(app.confirmBuf));
        }
    }

    // 检查完成
    {
        std::lock_guard<std::mutex> guard(app.shared.mu);
        if (app.shared.done) {
            app.running = false;
            app.lastKind = app.kind;
            app.kind = JobKind::None;
            app.lastOk = app.shared.result.ok;
            app.lastWroteDisk = app.shared.result.wroteDisk;
            if (app.worker.joinable()) app.worker.join();
        }
    }
}

// ---- 外观：Catppuccin Mocha + 触摸尺度 --------------------------------------
//
// 调色板取 Catppuccin Mocha。触摸尺度：ImGui 的命中盒由 FramePadding 决定
// （按钮/复选框/输入框的行高 = 字号 + 2*FramePadding.y），因此按"手指"而非
// "鼠标"定基准值，把每个控件撑到 ~44px 触控目标以上；TouchExtraPadding 再
// 给相邻控件留防误触间隔；滚动条加宽到可直接拖。
//
// 注意顺序：先设基准值再 ScaleAllSizes(dpiScale)，否则高 DPI 下两套缩放打架。

ImVec4 Rgb(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                  ((rgb & 0xFF) / 255.0f), a);
}

namespace mocha {
constexpr unsigned Crust = 0x11111B, Mantle = 0x181825, Base = 0x1E1E2E;
constexpr unsigned Surface0 = 0x313244, Surface1 = 0x45475A, Surface2 = 0x585B70;
constexpr unsigned Overlay0 = 0x6C7086, Overlay2 = 0x9399B2;
constexpr unsigned Text = 0xCDD6F4, Subtext0 = 0xA6ADC8;
constexpr unsigned Lavender = 0xB4BEFE, Blue = 0x89B4FA;
constexpr unsigned Sky = 0x89DCEB, Teal = 0x94E2D5;
constexpr unsigned Green = 0xA6E3A1, Yellow = 0xF9E2AF, Peach = 0xFAB387;
constexpr unsigned Red = 0xF38BA8, Maroon = 0xEBA0AC, Mauve = 0xCBA6F7;
constexpr unsigned Rosewater = 0xF5E0DC;
}  // namespace mocha

void ApplyCatppuccinMocha(float dpiScale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // —— 触摸尺度基准 ——
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(18, 13);   // 控件命中盒高度 ≈ 字号 + 26
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.CellPadding = ImVec2(12, 10);
    s.TouchExtraPadding = ImVec2(8, 8);  // 相邻控件的额外防误触间隔
    s.ScrollbarSize = 24;
    s.GrabMinSize = 24;
    s.WindowRounding = 8;
    s.ChildRounding = 8;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 8;
    s.TabRounding = 6;

    // —— Mocha 调色 ——
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Rgb(mocha::Text);
    c[ImGuiCol_TextDisabled] = Rgb(mocha::Overlay0);
    c[ImGuiCol_WindowBg] = Rgb(mocha::Base);
    c[ImGuiCol_ChildBg] = Rgb(mocha::Base);
    c[ImGuiCol_PopupBg] = Rgb(mocha::Mantle);
    c[ImGuiCol_Border] = Rgb(mocha::Surface0);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = Rgb(mocha::Surface0);
    c[ImGuiCol_FrameBgHovered] = Rgb(mocha::Surface1);
    c[ImGuiCol_FrameBgActive] = Rgb(mocha::Surface2);
    c[ImGuiCol_TitleBg] = Rgb(mocha::Mantle);
    c[ImGuiCol_TitleBgActive] = Rgb(mocha::Surface0);
    c[ImGuiCol_TitleBgCollapsed] = Rgb(mocha::Mantle);
    c[ImGuiCol_MenuBarBg] = Rgb(mocha::Mantle);
    c[ImGuiCol_ScrollbarBg] = Rgb(mocha::Crust);
    c[ImGuiCol_ScrollbarGrab] = Rgb(mocha::Surface2);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(mocha::Overlay0);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(mocha::Overlay2);
    c[ImGuiCol_CheckMark] = Rgb(mocha::Crust);
    // 1.92 新增：勾选态复选框底色（不设就继承 StyleColorsDark 的亮蓝）
    c[ImGuiCol_CheckboxSelectedBg] = Rgb(mocha::Blue);
    c[ImGuiCol_SliderGrab] = Rgb(mocha::Lavender);
    c[ImGuiCol_SliderGrabActive] = Rgb(mocha::Rosewater);
    c[ImGuiCol_Button] = Rgb(mocha::Surface1);
    c[ImGuiCol_ButtonHovered] = Rgb(mocha::Surface2);
    c[ImGuiCol_ButtonActive] = Rgb(mocha::Surface0);
    c[ImGuiCol_Header] = Rgb(mocha::Surface0);
    c[ImGuiCol_HeaderHovered] = Rgb(mocha::Surface1);
    c[ImGuiCol_HeaderActive] = Rgb(mocha::Surface2);
    c[ImGuiCol_Separator] = Rgb(mocha::Surface1);
    c[ImGuiCol_SeparatorHovered] = Rgb(mocha::Surface2);
    c[ImGuiCol_SeparatorActive] = Rgb(mocha::Overlay0);
    c[ImGuiCol_ResizeGrip] = Rgb(mocha::Surface1);
    c[ImGuiCol_ResizeGripHovered] = Rgb(mocha::Surface2);
    c[ImGuiCol_ResizeGripActive] = Rgb(mocha::Overlay0);
    c[ImGuiCol_TabHovered] = Rgb(mocha::Surface1);
    c[ImGuiCol_Tab] = Rgb(mocha::Mantle);
    c[ImGuiCol_TabSelected] = Rgb(mocha::Surface0);
    c[ImGuiCol_TabSelectedOverline] = Rgb(mocha::Lavender);
    c[ImGuiCol_TabDimmed] = Rgb(mocha::Mantle);
    c[ImGuiCol_TabDimmedSelected] = Rgb(mocha::Surface0);
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(mocha::Surface1);
    c[ImGuiCol_InputTextCursor] = Rgb(mocha::Rosewater);
    c[ImGuiCol_TableHeaderBg] = Rgb(mocha::Surface0);
    c[ImGuiCol_TableBorderStrong] = Rgb(mocha::Surface1);
    c[ImGuiCol_TableBorderLight] = Rgb(mocha::Surface0);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Rgb(mocha::Mantle, 0.6f);
    c[ImGuiCol_TextLink] = Rgb(mocha::Blue);
    c[ImGuiCol_TextSelectedBg] = Rgb(mocha::Lavender, 0.35f);
    c[ImGuiCol_TreeLines] = Rgb(mocha::Surface2);
    c[ImGuiCol_DragDropTarget] = Rgb(mocha::Lavender);
    c[ImGuiCol_DragDropTargetBg] = Rgb(mocha::Base, 0.5f);
    c[ImGuiCol_UnsavedMarker] = Rgb(mocha::Peach);
    c[ImGuiCol_NavCursor] = Rgb(mocha::Lavender);
    c[ImGuiCol_NavWindowingHighlight] = Rgb(mocha::Text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.65f);
    c[ImGuiCol_PlotLines] = Rgb(mocha::Blue);
    c[ImGuiCol_PlotLinesHovered] = Rgb(mocha::Sky);
    c[ImGuiCol_PlotHistogram] = Rgb(mocha::Green);
    c[ImGuiCol_PlotHistogramHovered] = Rgb(mocha::Teal);

    s.ScaleAllSizes(dpiScale);
}

// ---- D3D9 设备管理 ----------------------------------------------------------

static LPDIRECT3D9 g_pD3D = nullptr;
static LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr;
static bool g_DeviceLost = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static D3DPRESENT_PARAMETERS g_d3dpp = {};

bool CreateDeviceD3D(HWND hWnd) {
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr) return false;

    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                             D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp,
                             &g_pd3dDevice) < 0)
        return false;
    return true;
}

void CleanupDeviceD3D() {
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
    if (g_pD3D) {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
}

void ResetDevice() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL) IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}

// ---- Win32 消息循环 ---------------------------------------------------------

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            // 运行中禁止关闭窗口（防止 worker 线程持有驱动句柄时进程退出导致蓝屏）
            if (g_app && g_app->running) {
                ::MessageBoxW(hWnd, L"任务正在运行，请等待完成后再关闭。", L"SecMelt", MB_OK | MB_ICONWARNING);
                return 0;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- ImGui 主界面 -----------------------------------------------------------

void RenderUI(App& app) {
    ImGuiIO& io = ImGui::GetIO();

    // 名单文件动态加载：文件一变就重载（按钮在"目标探测"标题行）
    TickTargetsReload(app, ImGui::GetTime());

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.9f, 0.9f, 1.0f, 1.0f), "SecMelt");
    ImGui::SameLine();
    ImGui::TextDisabled("—— 裸盘级冻结解除");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 左右 1:1：左 = 可用宽度的一半（扣掉中间那道 ItemSpacing）
    const float paneGap = ImGui::GetStyle().ItemSpacing.x;
    const float paneW = (ImGui::GetContentRegionAvail().x - paneGap) * 0.5f;

    // 左侧：系统信息、资产清单、目标探测
    ImGui::BeginChild("LeftPane", ImVec2(paneW, 0), true);
    {
        ImGui::TextColored(Rgb(mocha::Lavender), "系统信息");
        ImGui::Spacing();
        ImGui::Text("操作系统: %s", app.osVersion.c_str());
        ImGui::Text("提权状态: %s", app.elevated ? "已提权" : "未提权（按钮禁用）");
        ImGui::TextWrapped("程序目录: %s", app.exeDirUtf8.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(Rgb(mocha::Lavender), "运行期资产");
        ImGui::Spacing();
        // 每行两个资产（项目|状态|项目|状态）：8 项 4 行。只读清单不需要触摸尺寸，
        // 而 1280x800@150% 这样的窗口里 8 行会把下面的目标表整块挤没。
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                            ImVec2(12.0f * app.dpiScale, 4.0f * app.dpiScale));
        {
            constexpr int kPerRow = 2;
            const int rows = (static_cast<int>(app.assets.size()) + kPerRow - 1) / kPerRow;
            if (ImGui::BeginTable("##Assets", kPerRow * 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                for (int r = 0; r < rows; ++r) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < kPerRow; ++c) {
                        const int idx = r * kPerRow + c;
                        if (idx >= static_cast<int>(app.assets.size())) {
                            ImGui::TableNextColumn();
                            ImGui::TableNextColumn();
                            continue;
                        }
                        const auto& a = app.assets[idx];
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", a.label);
                        ImGui::TableNextColumn();
                        if (a.present) {
                            ImGui::TextColored(Rgb(mocha::Green), "present");
                        } else {
                            ImGui::TextColored(Rgb(mocha::Red), "MISSING");
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 标题/来源文字与"重新加载名单"按钮同排时，按 frame 高度对齐基线，
        // 否则触摸尺寸的按钮（56+ px 高）会让贴顶的标题看起来错位。
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Rgb(mocha::Lavender), "目标探测（只读）");
        ImGui::SameLine();
        if (ImGui::Button("重新加载名单")) {
            RefreshTargets(app, secmelt::ExeDir(), true);
        }
        ImGui::SameLine();
        if (app.targetsFile.empty()) {
            ImGui::TextDisabled("内置默认（未找到 targets.txt）");
        } else {
            ImGui::TextDisabled("%s", secmelt::Narrow(app.targetsFile.filename().wstring()).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", secmelt::Narrow(app.targetsFile.wstring()).c_str());
            }
        }
        ImGui::Spacing();
        // 高度吃掉剩余空间：内容与容器等高 → 左栏不出现多余滚动条。
        // slack 是固定开销（表格外围边框等推算不到的部分），用「上次施加量 + 本次残余」
        // 迭代到不动点；直接拿实测值反馈会两帧振荡（0 → 75 → 0 → …）。
        static float slack = 0.0f;
        const float applied = slack;
        const float availY = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y;
        const float minH = 170.0f * app.dpiScale;
        const bool clamped = (availY - applied) < minH;
        const float targetsH = clamped ? minH : (availY - applied);
        if (ImGui::BeginTable("##Targets", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, targetsH))) {
            // 名称/标签列宽随内容（避免中文被裁），状态列吃余量并折行
            ImGui::TableSetupColumn("名称");
            ImGui::TableSetupColumn("标签");
            ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& t : app.targets) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", t.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", t.label.c_str());
                ImGui::TableNextColumn();
                if (t.present) {
                    ImGui::TextColored(Rgb(mocha::Peach), "%s", t.status.c_str());
                } else {
                    ImGui::TextDisabled("%s", t.status.c_str());
                }
            }
            ImGui::EndTable();
        }
        // 实测残余溢出：迭代到不动点（被 min 高度夹住时不再累积，避免无界增长）
        if (!clamped) slack = applied + ImGui::GetScrollMaxY();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧：动作按钮、日志视图
    ImGui::BeginChild("RightPane", ImVec2(0, 0), true);
    {
        ImGui::TextColored(Rgb(mocha::Lavender), "动作");
        ImGui::Spacing();

        const bool canAct = app.elevated && !app.running;
        const float btnH = 56.0f * app.dpiScale;  // 触摸目标：整行可点

        ImGui::BeginDisabled(!canAct);

        if (ImGui::Button("预检扫描 (--preflight)", ImVec2(-FLT_MIN, btnH))) {
            StartJob(app, JobKind::Preflight);
        }
        ImGui::TextDisabled("装驱动 + 四层探针读写校验（只在带快照的 VM 里跑）");
        ImGui::Spacing();

        if (ImGui::Button("只读预演 (--dry-run)", ImVec2(-FLT_MIN, btnH))) {
            StartJob(app, JobKind::DryRun);
        }
        ImGui::TextDisabled("全链路预演，不写裸盘");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, Rgb(mocha::Red));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(mocha::Rosewater));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(mocha::Maroon));
        ImGui::PushStyleColor(ImGuiCol_Text, Rgb(mocha::Crust));
        if (ImGui::Button("执行 MELT (--melt)", ImVec2(-FLT_MIN, btnH))) {
            StartJob(app, JobKind::Melt);
        }
        ImGui::PopStyleColor(4);
        ImGui::TextDisabled("写回 hive 并置 dirty 标记");

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Checkbox("运行 ntfsfix（置 $LogFile dirty）", &app.runNtfsFix);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (app.running) {
            ImGui::TextColored(Rgb(mocha::Yellow), "运行中: %s", JobName(app.kind));
        } else if (app.lastKind != JobKind::None) {
            if (app.lastOk) {
                ImGui::TextColored(Rgb(mocha::Green), "上次任务: %s (成功)",
                                   JobName(app.lastKind));
            } else {
                ImGui::TextColored(Rgb(mocha::Red), "上次任务: %s (失败)",
                                   JobName(app.lastKind));
            }
            if (app.lastWroteDisk) {
                ImGui::SameLine();
                ImGui::TextColored(Rgb(mocha::Maroon), "[已动过裸盘]");
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(Rgb(mocha::Lavender), "日志");
        ImGui::Spacing();

        ImGui::BeginChild("##Log", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            for (const auto& line : app.lines) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LineColor(line)), "%s",
                                   line.c_str());
            }
            if (app.follow && !app.lines.empty()) {
                ImGui::SetScrollHereY(1.0f);
                app.follow = false;
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::End();

    // confirm 模态框
    if (app.confirmOpen) {
        ImGui::OpenPopup("确认写入");
        if (ImGui::BeginPopupModal("确认写入", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextColored(Rgb(mocha::Yellow), "即将写入裸盘。请确认以下动作：");
            ImGui::Spacing();
            for (const auto& item : app.confirmItems) {
                ImGui::BulletText("%s", item.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("键入 MELT 并点击确认以继续，或点击取消。");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(360.0f * app.dpiScale);
            ImGui::InputText("##ConfirmInput", app.confirmBuf, sizeof(app.confirmBuf));
            ImGui::Spacing();

            const bool match = (std::strcmp(app.confirmBuf, "MELT") == 0);
            const float modalBtn = 52.0f * app.dpiScale;

            if (match) {
                ImGui::PushStyleColor(ImGuiCol_Button, Rgb(mocha::Red));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(mocha::Rosewater));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(mocha::Maroon));
                ImGui::PushStyleColor(ImGuiCol_Text, Rgb(mocha::Crust));
            } else {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("确认", ImVec2(160 * app.dpiScale, modalBtn))) {
                std::lock_guard<std::mutex> guard(app.shared.mu);
                app.shared.confirmAnswer = true;
                app.shared.confirmAnswered = true;
                app.shared.cv.notify_all();
                app.confirmOpen = false;
                ImGui::CloseCurrentPopup();
            }

            if (match) {
                ImGui::PopStyleColor(4);
            } else {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(160 * app.dpiScale, modalBtn))) {
                std::lock_guard<std::mutex> guard(app.shared.mu);
                app.shared.confirmAnswer = false;
                app.shared.confirmAnswered = true;
                app.shared.cv.notify_all();
                app.confirmOpen = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}

}  // namespace

// ---- 入口点 -----------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // DPI 感知
    ImGui_ImplWin32_EnableDpiAwareness();
    float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    // 创建窗口
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC,     WndProc, 0L,     0L, hInstance, nullptr,
                      nullptr,     nullptr,        nullptr, L"SecMelt", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"SecMelt — 裸盘级冻结解除", WS_OVERLAPPEDWINDOW,
                                100, 100, (int)(1280 * mainScale), (int)(800 * mainScale),
                                nullptr, nullptr, wc.hInstance, nullptr);

    // 初始化 D3D9
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // 初始化 ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 布局是固定单窗口，不需要持久化；默认会在**工作目录**写出 imgui.ini
    io.IniFilename = nullptr;

    ApplyCatppuccinMocha(mainScale);
    ImGui::GetStyle().FontScaleDpi = mainScale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    // CJK 字体：1.92 起为动态字形加载（DX9 后端已置 ImGuiBackendFlags_RendererHasTextures），
    // 因此**不需要**预烘焙 glyph ranges —— 用到哪个字才加载哪个。字体目录按
    // GetWindowsDirectoryW 取（系统盘不一定是 C:）。先探存在再加载：AddFontFromFileTTF
    // 对缺失文件会走 IM_ASSERT_USER_ERROR，debug 构建下会中断。
    {
        wchar_t winDir[MAX_PATH] = {};
        ::GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";
        const wchar_t* kCandidates[] = {L"msyh.ttc", L"msyh.ttf", L"Deng.ttf", L"simhei.ttf",
                                        L"simsun.ttc"};
        for (const wchar_t* name : kCandidates) {
            const fs::path candidate = fs::path(fontsDir) / name;
            if (!secmelt::PathIsRegularFile(candidate)) continue;
            if (io.Fonts->AddFontFromFileTTF(secmelt::Narrow(candidate.wstring()).c_str(),
                                             18.0f) != nullptr)
                break;
        }
        // 全部缺失时回落内置字体（仅 ASCII）：界面仍可用，中文显示为占位符。
    }

    // 初始化 App 状态
    App app;
    app.hwnd = hwnd;
    app.dpiScale = mainScale;
    app.elevated = secmelt::IsProcessElevated();
    app.osVersion = WindowsVersionString();
    const fs::path exeDir = secmelt::ExeDir();
    app.exeDirUtf8 = secmelt::Narrow(exeDir.wstring());
    RefreshAssets(app, exeDir);
    g_app = &app;

    UiLog(app, "[*] SecMelt GUI initialized");
    if (!app.elevated) {
        UiLog(app, "[!] NOT elevated: all action buttons are disabled");
    }
    RefreshTargets(app, exeDir, true);

    // 主循环
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 处理设备丢失
        if (g_DeviceLost) {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) {
                ::Sleep(10);
                continue;
            }
            if (hr == D3DERR_DEVICENOTRESET) ResetDevice();
            g_DeviceLost = false;
        }

        // 处理窗口 resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            g_d3dpp.BackBufferWidth = g_ResizeWidth;
            g_d3dpp.BackBufferHeight = g_ResizeHeight;
            g_ResizeWidth = g_ResizeHeight = 0;
            ResetDevice();
        }

        // 轮询 worker 线程
        PollWorker(app);

        // 渲染帧
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI(app);

        ImGui::EndFrame();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                            D3DCOLOR_RGBA(18, 20, 24, 255), 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui::Render();
            ImDrawData* drawData = ImGui::GetDrawData();
            if (drawData != nullptr) {
                ImGui_ImplDX9_RenderDrawData(drawData);
            }
            g_pd3dDevice->EndScene();
        }
        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST) g_DeviceLost = true;
    }

    // 清理
    if (app.worker.joinable()) app.worker.join();

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
