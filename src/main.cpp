// SecMelt —— 命令行前端（无交互界面）
//
// 每条子命令都是"跑完打印报告就退出"的进程，不需要任何可交互终端 —— 这正是 Windows 7
// 原生 conhost（不解释 VT 转义序列）上唯一能工作的形态。需要点按式交互时用图形前端
// secmelt-gui（源码在 src/gui），它与这里共用同一份 pipeline 源码。
//
// 用法：
//   secmelt --help          本说明
//   secmelt --dump          自检报告：环境 + 构建期资产 + 名单的实测存在状态（只读，不碰盘）
//   secmelt --dry-run       只读预演整条链路：预检 + 只读注册表探测 + 导出/校验 hive
//   secmelt --preflight     melt 前的根因扫描（logstate 守卫 + 目录层级探针 + 拆解报告）
//   secmelt --melt --yes-i-know 非交互执行整条链路（破坏性；不自动复位）
//   secmelt --selftest-hive 校验 hive base block 偏移与校验和算法
//   secmelt --selftest-registry 在 scratch 键上验证过滤器摘除的写入路径
//   secmelt --selftest-raw  裸盘写入/读回判定（会装载内核驱动、写系统卷，仅限虚拟机）
//
// 文案刻意保持 ASCII —— Windows 控制台默认代码页多为 936(GBK)，非 ASCII 文案在部分
// 终端会花屏；宽字符只用于 Win32 API 边界与 UTF-8 名单文件。着色规则见 util.h::CliPaint。

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "melt/unfreeze.h"
#include "raw/win_disk.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "selftest.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

struct Check {
    std::string label;
    bool ok = false;
    std::string detail;
    // 失败时给出的下一步动作：故障不能只报状态，还要告诉人该干什么。
    std::string hint;
};

// 所有 CLI 输出都从这里走：着色规则（出错醒目红色、[!] 琥珀、其余白色）与
// --preflight/--selftest-* 是同一套，判定在 util.cpp::CliPaint 里。
void Print(const std::wstring& line) {
    std::cout << secmelt::Narrow(secmelt::CliPaint(line)) << "\n";
}

std::wstring W(const std::string& s) { return secmelt::Widen(s); }

// 一行检查结果：`[+] 标签  细节`；不通过时再补一行处置动作。
void PrintCheck(const Check& c) {
    Print(secmelt::FormatW(L"%ls %-34ls %ls", c.ok ? L"[+]" : L"[x]", W(c.label).c_str(),
                           W(c.detail).c_str()));
    if (!c.ok && !c.hint.empty()) Print(secmelt::FormatW(L"    -> %ls", W(c.hint).c_str()));
}

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

// 用一次真实的装载尝试来判断"签名强制有没有在拦"，被拦下就 kdu -dse 0 再复测 ——
// 与 melt 的第一步共用同一份实现（melt/unfreeze.h::EnsureUnsignedDriverLoads），
// 所以报告里写的是**实测结论**，不是"melt 之后大概会怎样"的预测。
//
// 为什么不用 NtQuerySystemInformation(SystemCodeIntegrityInformation)：它读到的
// CodeIntegrityOptions 来自启动期的配置（BCD 一类）映像，不是内核此刻的实际拦截状态 ——
// 实测有机器报 "enabled" 却根本不拦，也有报 1 而确实在拦。装载返回码才是判据：
//   577 (ERROR_INVALID_IMAGE_HASH) = 还在拦；装上了 = 没在拦。
// WDAC 策略与易受攻击驱动黑名单同样表现为 577，所以这一项测的是"能不能装"这件事本身，
// 而不只是"DSE 开没开"——那正好就是 melt 下一步要依赖的事实。
//
// allowDseFix=false（--dump）：只探测不动系统，报告里说清"这一步由 Melt 完成"。
secmelt::DriverLoadOutcome ProbeDriverLoad(const fs::path& exeDir, bool allowDseFix) {
    return secmelt::EnsureUnsignedDriverLoads(exeDir, allowDseFix);
}

std::vector<Check> RunEnvironmentChecks(bool allowDseFix) {
    std::vector<Check> out;

    const bool elevated = secmelt::IsProcessElevated();
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

    // 签名强制这项不查任何接口 —— 直接试着装载未签名的 WinDisk.sys，看返回码。
    // 非提权时装载必然失败（OpenSCManager 拒绝），那种失败说明不了 DSE，如实报"未探测"。
    if (!elevated) {
        out.push_back({"Driver signature enforcement", true,
                       "not probed (the load probe needs an elevated prompt)", ""});
    } else {
        const secmelt::DriverLoadOutcome load = ProbeDriverLoad(secmelt::ExeDir(), allowDseFix);
        switch (load.result) {
            case secmelt::DriverLoad::Loaded:
                if (load.kduRan) {
                    // 这次探测自己把 DSE 关掉了 —— 这是结论，不是预测。
                    out.push_back({"Driver signature enforcement", true,
                                   "was blocking; turned off with 'kdu -dse 0' and the load now "
                                   "succeeds", ""});
                } else {
                    // 未签名的驱动装上了（或本来就在运行）—— 没有东西在拦它。
                    out.push_back({"Driver signature enforcement", true,
                                   "not blocking: the unsigned WinDisk.sys is loaded", ""});
                }
                break;
            case secmelt::DriverLoad::SignatureRejected:
                // 想关但没关成：要么被允许去关却没成功，要么这条路径不许动系统。
                if (load.kduRan) {
                    out.push_back({"Driver signature enforcement", false,
                                   "the load is still rejected with 577 even after 'kdu -dse 0' "
                                   "(HVCI / memory integrity on, a WDAC policy, or the Microsoft "
                                   "vulnerable driver blocklist)",
                                   "turn off Memory integrity in Windows Security, or check "
                                   "the blocklist"});
                } else {
                    out.push_back({"Driver signature enforcement", true,
                                   "blocking: the load was rejected with 577 (ERROR_INVALID_IMAGE_HASH); "
                                   "Melt runs 'kdu -dse 0' and retries the load",
                                   ""});
                }
                break;
            case secmelt::DriverLoad::Failed:
                // 577 以外的失败与签名无关（服务注册、驱动文件、权限…），要人去处理。
                out.push_back({"Driver signature enforcement", false,
                               secmelt::Narrow(load.firstError),
                               "run: secmelt --selftest-raw (in a VM) to see the full error"});
                break;
        }
    }

    return out;
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

    // 部署包（拿来就能跑的 release 目录）里没有源码树 —— 上面这些都是**构建期**资产，
    // 运行时不需要它们（运行时要的是 exe 旁边的 WinDisk_x64.sys / kdu.exe / drv64.dll /
    // targets.txt / tools）。源码树不在时只报一行，否则部署包里会刷出 6 条"缺失"故障，
    // 把真正需要人处理的问题淹掉。
    if (!secmelt::PathIsDirectory(root / "third_party")) {
        return {{"Source tree", true,
                 "not present (deployed bundle: build-time assets are not checked here)", ""}};
    }

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

// --dump：把三项自检（环境 / 构建期资产 / 名单实测状态）打成纯文本报告。
//
// 这是 CI 与无 TTY 环境下的回归入口：不装驱动（只探测装载）、不碰盘、不改注册表。
// 退出码 = 环境与资产是否全绿；"名单里有东西存在"是预期状态而不是故障，不计入退出码
// （它恰恰是 melt 要处理的东西）。
int DumpMode(const fs::path& root) {
    // 运行期资产（targets.txt）相对 exe 目录解析，源码树根 root 只用于构建期资产自检
    // 与报告里的路径显示 —— 两者在部署包里不是一回事。
    const fs::path exeDir = secmelt::ExeDir();
    // --dump 是纯报告：探测装载（判 DSE），但**不动系统** —— 不去调用 kdu。
    const auto environment = RunEnvironmentChecks(/*allowDseFix=*/false);
    const auto assets = RunAssetChecks(root);
    const auto rows = ProbeTargetRows(exeDir);

    Print(L" SecMelt :: Melt - self-check (dump mode)");
    Print(secmelt::FormatW(L" %ls  checks executed", secmelt::Timestamp().c_str()));
    Print(secmelt::FormatW(L" project root : %ls", root.c_str()));
    Print(secmelt::FormatW(L" exe dir      : %ls", exeDir.c_str()));

    size_t failed = 0;
    Print(L"--- environment ---");
    for (const auto& c : environment) {
        PrintCheck(c);
        if (!c.ok) ++failed;
    }
    Print(L"--- build-time assets ---");
    for (const auto& c : assets) {
        PrintCheck(c);
        if (!c.ok) ++failed;
    }

    Print(L"--- targets: state on this machine ---");
    for (const auto& row : rows) {
        // 名单里的项存在 = 需要处理，用 [x] 让它在支持 ANSI 的终端里醒目（红）。
        // 只给 ASCII 的 name 列做定宽：label 是中文，按 UTF-16 单元补齐反而会对不齐。
        Print(secmelt::FormatW(L"%ls %-16ls %ls  %ls", row.present ? L"[x]" : L"[ ]",
                               W(row.name).c_str(), W(row.label).c_str(), W(row.status).c_str()));
    }

    Print(L"--- summary ---");
    if (failed == 0) {
        Print(L"[+] ALL SYSTEMS GO");
    } else {
        Print(secmelt::FormatW(L"[x] CHECK FAILED: %zu unsatisfied", failed));
    }
    return failed == 0 ? 0 : 1;
}

void PrintUsage() {
    std::cout << "SecMelt - command line front end (the graphical front end is secmelt-gui)\n"
                 "  secmelt --dump           self-check report, then exit (no TTY needed)\n"
                 "  secmelt --dry-run        rehearse the whole chain without writing the disk\n"
                 "  secmelt --preflight      one-shot root-cause scan: logstate + directory\n"
                 "                           tier probing + DiagnoseWritePath breakdown, then stop\n"
                 "  secmelt --melt --yes-i-know\n"
                 "                           non-interactive apply: strip filters, write the hive\n"
                 "                           (and its RegBack copy), then reboot the machine by hand\n"
                 "                           once it reports the writes verified (no auto reset)\n"
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
    bool selftestHive = false;
    bool selftestRegistry = false;
    bool selftestRaw = false;
    bool dump = false;
    bool help = false;
    bool melt = false;
    bool preflight = false;
    bool yesIKnow = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dump") dump = true;
        else if (arg == "--dry-run") dryRun = true;
        else if (arg == "--melt") melt = true;
        else if (arg == "--preflight") preflight = true;
        else if (arg == "--yes-i-know") yesIKnow = true;
        else if (arg == "--selftest-hive") selftestHive = true;
        else if (arg == "--selftest-registry") selftestRegistry = true;
        else if (arg == "--selftest-raw") selftestRaw = true;
        else if (arg == "--help" || arg == "-h") help = true;
        else {
            std::cout << secmelt::Narrow(
                             secmelt::CliPaint(L"unknown option: " + secmelt::Widen(std::string(arg))))
                      << "\n";
            PrintUsage();
            return 2;
        }
    }

    // 没有可交互界面了：不给子命令就只打印用法。--help 是显式请求，退出码 0；
    // 空手调用按"缺命令"处理，让脚本不会把一次空转当成成功。
    if (help || argc < 2) {
        PrintUsage();
        return help ? 0 : 2;
    }

    const fs::path root = FindProjectRoot();
    const fs::path exeDir = secmelt::ExeDir();

    if (melt) {
        // 落盘不可逆：没有显式 token 就不执行（这是唯一能执行落盘的入口）。
        if (!yesIKnow) {
            std::cout << "--melt writes the SYSTEM hive to the raw disk (and its RegBack copy).\n"
                         "There is no rollback on this machine (System Restore cannot undo it);\n"
                         "only a VM snapshot can. Nothing is reset automatically: the machine is\n"
                         "rebooted by you once the writes verify.\n"
                         "Re-run with --melt --yes-i-know if that is what you want.\n";
            return 2;
        }
        return secmelt::MeltApply(exeDir);
    }
    if (preflight) return secmelt::PreflightScan(exeDir);
    if (selftestHive) return secmelt::HiveSelfTest(exeDir);
    if (selftestRegistry) return secmelt::RegistrySelfTest(exeDir);
    if (selftestRaw) return secmelt::RawSelfTest(exeDir);
    if (dryRun) return secmelt::MeltDryRun(exeDir.empty() ? root : exeDir);
    if (dump) return DumpMode(root);

    // 只给了修饰选项（如 --yes-i-know）而没有动作：同样按缺命令处理。
    PrintUsage();
    return 2;
}
