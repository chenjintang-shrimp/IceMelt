#include "melt/unfreeze.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

#include "raw/ntfs_tool.h"
#include "raw/win_disk.h"
#include "reg/hive.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

namespace secmelt {
namespace {

constexpr const wchar_t* kServiceName = L"WinDisk";
constexpr const wchar_t* kDefaultSysName = L"WinDisk_x64.sys";
constexpr const wchar_t* kSystemVolume = L"\\\\.\\C:";
// 卷内路径（ntfscp 的 dest 参数，相对卷根；与原作者 ReplaceFileNTFS 的构造方式一致）
constexpr const wchar_t* kNtfsSystemHive = L"\\Windows\\System32\\config\\SYSTEM";
// 同一个文件的 Win32 路径：FSCTL_GET_RETRIEVAL_POINTERS 只能对真实路径用，
// 读回校验要拿它的物理区段（卷内路径只对 ntfscp 有意义）。
constexpr const wchar_t* kSystemHiveWin32 = L"C:\\Windows\\System32\\config\\SYSTEM";
// SYSTEM hive 的备份副本。Windows 在 RegBack 下留一份（Win7 有；Win10 1803 起默认清空），
// 启动修复做"修复"时会拿它比对/还原 —— 所以这一组文件的代数必须一致（见第 8b 步）。
constexpr const wchar_t* kHiveBackupWin32 = L"C:\\Windows\\System32\\config\\RegBack\\SYSTEM";
constexpr const wchar_t* kNtfsHiveBackup = L"\\Windows\\System32\\config\\RegBack\\SYSTEM";
// SYSTEM 的事务日志。三份都要处理：
//   * .LOG1 / .LOG2 是 Vista 起的双日志方案（写主 hive 失败时切换到另一份）；
//   * .LOG 在双日志方案下通常是安装镜像留下的空壳，但单日志方案下它就是唯一那份 ——
//     只处理 LOG1/LOG2 会漏掉后者（regf 规范「Multiple transaction log files」一节）。
constexpr const wchar_t* kLogFiles[] = {
    L"C:\\Windows\\System32\\config\\SYSTEM.LOG",
    L"C:\\Windows\\System32\\config\\SYSTEM.LOG1",
    L"C:\\Windows\\System32\\config\\SYSTEM.LOG2",
};
// 与 third_party/WinDisk/Main.cpp 的 SECMELT_BUGCHECK_CODE 必须一致：
// CTL_REBOOT_SYSTEM 触发这个 bugcheck，而不是走常规关机路径。
constexpr unsigned long kBugCheckCode = 0x0D000721;

struct Log {
    MeltResult* result;  // 可为空：只有 sink 的日志也是合法的（例如把 kdu 输出收进一个字符串）
    MeltLogger sink;
    void operator()(const std::wstring& line) const {
        if (result) result->log.push_back(line);
        if (sink) sink(line);
    }
    void step(const std::wstring& line) const { (*this)(FormatW(L"[*] %ls", line.c_str())); }
    void ok(const std::wstring& line) const { (*this)(FormatW(L"[+] %ls", line.c_str())); }
    void warn(const std::wstring& line) const { (*this)(FormatW(L"[!] %ls", line.c_str())); }
};

std::wstring Fail(const std::wstring& what, const std::wstring& error) {
    return FormatW(L"[x] %ls: %ls", what.c_str(), error.c_str());
}

// 把 base block 压成一行，日志与自检共用
std::wstring DescribeBaseBlock(const HiveBaseBlock& b) {
    return FormatW(
        L"sig=%ls seq1=%u seq2=%u ver=%u.%u type=%u format=%u root=%u bins=%u cluster=%u "
        L"stored=0x%08X computed=0x%08X size=%llu clean=%ls checksumOk=%ls rootCellValid=%ls",
        b.signatureOk ? L"regf" : L"BAD", b.sequence1, b.sequence2, b.major, b.minor, b.type,
        b.format, b.rootCell, b.binsSize, b.clusteringFactor, b.storedChecksum, b.computedChecksum,
        b.fileSize, b.clean ? L"yes" : L"no", b.checksumOk ? L"yes" : L"no",
        b.rootCellValid ? L"yes" : L"no");
}

std::filesystem::path DefaultHivePath() {
    std::vector<wchar_t> temp(MAX_PATH);
    const DWORD need = ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    const std::filesystem::path dir = (need > 0 && need < temp.size())
                                          ? std::filesystem::path(temp.data())
                                          : std::filesystem::path(L"C:\\Windows\\Temp");
    return dir / "secmelt-system.hive";
}

// 跑 `kdu -dse 0`。本函数只负责把 kdu 拉起来并如实转发输出 —— **不判定 DSE 是否
// 真被解开**：判据是调用方随后的重新装载（见 win_disk.h::DriverLoad）。
//
// 两个实现要点：
//   * kdu 的退出码不可靠 —— 它的 ControlDSE 回调返回的是不同 provider 各自的 BOOL，
//     失败路径也可能返回 0。所以这里只检查进程是否跑起来、输出原样进日志。
//   * kdu 需要与它同目录的 drv64.dll（provider 数据库）。缺了它 kdu 会退回 Hamakaze 里
//     那份空的内嵌表，输出 `Provider: "(null)"` + `Driver resource id cannot be found`，
//     然后静默什么都不做 —— 所以这里先显式检查 drv64.dll，把这种情况变成一条明确的错误。
bool RunKduDseOff(const std::filesystem::path& exeDir, const Log& log, std::wstring& error) {
    std::filesystem::path kdu = exeDir / L"kdu.exe";
    if (!secmelt::PathIsRegularFile(kdu)) {
        const std::filesystem::path found = ResolveTool(L"kdu.exe");
        if (found.empty()) {
            error = L"kdu.exe not found (looked next to secmelt.exe, then <exeDir>/tools, then PATH)";
            return false;
        }
        kdu = found;
    }

    const std::filesystem::path db = kdu.parent_path() / L"drv64.dll";
    if (!secmelt::PathIsRegularFile(db)) {
        error = FormatW(L"drv64.dll is missing next to %ls -- kdu would silently do nothing "
                        L"without its provider database",
                        kdu.filename().c_str());
        return false;
    }

    log.step(FormatW(L"running: %ls -dse 0", kdu.filename().c_str()));
    int exitCode = -1;
    std::wstring output;
    if (!RunTool(kdu, L"-dse 0", exitCode, output)) {
        error = FormatW(L"failed to run %ls", kdu.filename().c_str());
        return false;
    }
    log.step(FormatW(L"kdu -dse 0 exited with %d (its exit code is not a DSE verdict)", exitCode));
    // kdu 的输出按行进日志（它的诊断信息对判断失败原因有用）
    size_t begin = 0;
    while (begin < output.size()) {
        const size_t end = output.find(L'\n', begin);
        std::wstring one(output, begin,
                         end == std::wstring::npos ? std::wstring::npos : end - begin);
        while (!one.empty() && (one.back() == L'\r' || one.back() == L' ')) one.pop_back();
        if (!one.empty()) log.step(one);
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return true;
}

// 触发重启 bugcheck。CTL_REBOOT_SYSTEM 走的是 KeBugCheckEx(0x0D000721)：
//   * 内核在此停住，不再把内存里的脏 hive 刷回磁盘（这正是写回 hive 之后必须走的路径）；
//   * 崩溃转储栈会先把内存映像直写磁盘，然后按系统的崩溃设置自动重启。
// 成功即不返回；返回 FALSE 说明 bugcheck 没能让机器复位。
bool Trip(const Log& log, WinDiskDevice& device, std::wstring& error) {
    log.warn(FormatW(L"triggering bugcheck 0x%08X via CTL_REBOOT_SYSTEM", kBugCheckCode));
    return device.RebootNow(error);
}

// 写回前的守卫：确认 **ntfs-3g 看到的**那个目标确实是 hive。
//
// 两点必须说清楚：
//   * 不能按 Win32 路径打开活动 SYSTEM hive —— 内核独占持有它，
//     CreateFileW 会返回 ERROR_SHARING_VIOLATION(32)（实测）。
//   * 也**不应该**只看运行中文件系统的视图：ntfscp 是按 ntfs-3g 的视图写盘的，两者不一致
//     正是"把内容写到别人的簇上"这类事故的机理。所以要校验的就是 ntfs-3g 的视图 —— 用它
//     自己的 readhead 把开头若干字节抄出来看。
//
// 判据简单且决定性：SYSTEM hive 以及它的日志（开头是 base block 的部分备份副本）都以
// ASCII "regf" 开头。不是的话**一个字节都不要写**，此时还没过不可回退点，能干净收场。
bool TargetHoldsHive(WinDiskDevice& device, const VolumeInfo& vol, const std::wstring& ntfsPath,
                     const std::filesystem::path& scratchFile, const Log& log) {
    int exitCode = -1;
    std::wstring output;
    std::wstring error;
    uint64_t targetSize = 0;
    if (!NtfsReadFile(device.raw(), vol, ntfsPath, scratchFile, kHiveBlockSize, exitCode, output,
                      error, &targetSize)) {
        log.warn(Fail(FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()), error));
        return false;
    }
    if (targetSize == 0) {
        // 0 字节不是"未知内容"，是"什么都没有" —— 覆盖它不会毁掉任何东西，反而是修复
        // （实测这台机器的 config\RegBack\SYSTEM 就是 0 字节，把这个位置放回一份有效 hive
        // 正是我们想要的）。所以空文件放行。
        // 必须放在 exitCode 检查**之前**：readhead 请求 4096 字节却只得到 0，会以"失败"退出。
        log.warn(FormatW(L"pre-write guard (%ls): the target is 0 bytes (nothing to destroy) -- "
                         L"allowed, writing will give it content",
                         ntfsPath.c_str()));
        return true;
    }
    if (exitCode != 0) {
        // 注意措辞：读不到**不是**关于内容的判断。之前这里直接把"读失败"说成
        // "the clusters ... do not hold a hive"，是一次误诊。
        log.warn(Fail(FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()),
                      FormatW(L"could not read the target through ntfs-3g (readhead exited %d): "
                              L"%ls -- this is not a verdict about its contents",
                              exitCode, output.c_str())));
        // 逐层 stat：ntfs_pathname_to_inode 会在每一层做 inode 查找，任一层缺失都只报
        // ENOENT。逐层来一遍就能看出**是哪一层**断的 —— 是 /Windows 不在这个视图里，
        // 还是最末一层 SYSTEM 不在。这是诊断的关键信息，不该让人再去猜。
        //
        // /Users 也在列表里，而且它是最有信息量的一个：secmelt.exe 自己就跑在
        // /Users/<user>/Desktop/... 下，所以**它必然存在于文件系统的视图里**。若连它都
        // 查不到（而 "/" 查得到），那就说明 ntfs-3g 读到的基线 $MFT 与运行中文件系统的
        // 视图确实不一致，而不是我们的查找写错了。
        for (const wchar_t* prefix : {L"\\", L"\\Windows", L"\\Users", L"\\Windows\\System32",
                                      L"\\Windows\\System32\\config", ntfsPath.c_str()}) {
            std::wstring detail;
            const bool present = NtfsStatPath(device.raw(), vol, prefix, detail);
            log.step(FormatW(L"  path probe %ls: %ls", prefix, detail.c_str()));
            (void)present;
        }

        // 名字查不到时，把**父目录实际有什么**摆出来。这能直接把
        // "索引里根本没这个名字" 与 "查找本身有问题" 分开：
        //   * 父目录列得出来、里面没有 SYSTEM  → 这个文件在卷的目录索引里已经不存在了
        //     （第一次 melt 的裸写打掉了它的索引项，或者启动修复动过）；
        //   * 父目录都列不出来             → 探针/挂载的问题，不是文件的问题。
        const std::wstring parent = std::wstring(ntfsPath).substr(0, ntfsPath.find_last_of(L'\\'));
        std::wstring listing;
        if (NtfsListDir(device.raw(), vol, parent, listing)) {
            // 只把名字挑出来，最多 40 个，免得日志被 200 行冲掉
            std::vector<std::wstring> names;
            size_t begin = 0;
            while (begin < listing.size() && names.size() < 40) {
                const size_t nl = listing.find(L'\n', begin);
                std::wstring line(listing, begin,
                                  nl == std::wstring::npos ? std::wstring::npos : nl - begin);
                const size_t tab = line.find(L'\t');
                if (tab != std::wstring::npos) {
                    names.push_back(line.substr(0, 1) + L" " + line.substr(tab + 1));
                }
                // 没有制表符的那行是工具自己的表头（"<path> (N entries):"）—— 丢掉，
                // 否则会拼进"目录内容"里，正是上一版日志里那行看着像乱码的东西。
                if (nl == std::wstring::npos) break;
                begin = nl + 1;
            }
            std::wstring joined;
            for (size_t i = 0; i < names.size(); ++i) joined += (i ? L", " : L"") + names[i];
            log.warn(FormatW(L"  contents of %ls: %ls%s", parent.c_str(), joined.c_str(),
                             names.size() >= 40 ? L", ..." : L""));
        } else {
            log.warn(FormatW(L"  could not list %ls: %ls", parent.c_str(), listing.c_str()));
        }
        return false;
    }

    // 复用已实现的读回比对：把它与 "开头必须是 regf" 的期望一起判掉
    std::ifstream in(scratchFile, std::ios::binary | std::ios::ate);
    std::vector<unsigned char> head;
    if (!in) {
        log.warn(Fail(FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()),
                      FormatW(L"cannot open %ls", scratchFile.c_str())));
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size == 0) {
        // 0 字节不是"未知内容"，是"什么都没有" —— 覆盖它不会毁掉任何东西，反而是修复
        // （实测这台机器的 config\RegBack\SYSTEM 就是 0 字节，把这个位置放回一份有效 hive
        // 正是我们想要的）。所以空文件放行。
        log.warn(FormatW(L"pre-write guard (%ls): the target is 0 bytes (nothing to destroy) -- "
                         L"allowed, writing will give it content",
                         ntfsPath.c_str()));
        return true;
    }
    if (size < 4) {
        log.warn(Fail(FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()),
                      FormatW(L"readhead returned only %lld bytes", static_cast<long long>(size))));
        return false;
    }
    head.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) {
        log.warn(Fail(FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()),
                      L"cannot read the readhead scratch file"));
        return false;
    }

    if (std::memcmp(head.data(), "regf", 4) != 0) {
        auto printable = [](unsigned char c) { return (c >= 32 && c < 127) ? static_cast<char>(c) : '.'; };
        log.warn(Fail(
            FormatW(L"pre-write guard (%ls)", ntfsPath.c_str()),
            FormatW(L"ntfs-3g does not see a hive here: first bytes are %02X %02X %02X %02X "
                    L"(\"%c%c%c%c\"). Refusing to write -- whatever this path actually holds, "
                    L"overwriting it would corrupt the volume. Nothing was written.",
                    head[0], head[1], head[2], head[3], printable(head[0]), printable(head[1]),
                    printable(head[2]), printable(head[3]))));
        return false;
    }

    // 顺手把 ntfs-3g 视图里那份**旧** hive 的 base block 记下来：这是"我们写之前这里是什么"
    // 的直接记录。文件长度取自 readhead 报告的 file size —— 只读前 4096 字节的话，文件长度
    // 就是未知的，root cell 校验会给出 "rootCellValid=no" 这种**误导性**结论（踩过一次）。
    HiveBaseBlock existing;
    if (ParseBaseBlock(head.data(), head.size(), targetSize, existing)) {
        log.step(L"pre-write guard: the hive ntfs-3g sees is " + DescribeBaseBlock(existing));
    }
    log.ok(FormatW(L"pre-write guard: ntfs-3g reads a regf hive at %ls -- safe to overwrite",
                   ntfsPath.c_str()));
    return true;
}

// 把"写路径本身就丢数据"与"只是 hive 这个文件（原地重写属性）出问题"分开。
//
// 做法：在**同一个卷**上，用**完全相同的那条写路径**（ntfscp → handle: → 驱动）写一个临时文件
// 再读回来。两边唯一不同的是"新文件"与"原地重写已有属性"，所以：
//   * 临时文件**干净往返** → 写路径没问题 → 失败与那个文件的元数据/布局有关；
//   * 临时文件**也脏** → 写路径本身就丢数据，与目标文件无关。
//
// 之前那版用的是"按运行中文件系统的区段经驱动读回来比" —— 那个设计不成立：readhead 读的是
// 同一份磁盘 $MFT 给出的同一批簇，比出来必然一致，证明不了任何事。（而且它用 Win32 路径取
// 区段，对活动 hive 直接 ERROR_SHARING_VIOLATION(32) 失败 —— 同一个坑踩第二次。）
void DiagnoseWritePath(WinDiskDevice& device, const VolumeInfo& vol,
                       const std::filesystem::path& exportFile,
                       const std::filesystem::path& scratchDir, const Log& log) {
    constexpr size_t kProbeSize = 4u << 20;
    constexpr unsigned char kProbeByte = 0x5A;

    // ---- 第一组：小文件、全新名字（内核没持有它）------------------------------
    {
        const std::filesystem::path localProbe = scratchDir / L"secmelt-writeprobe.bin";
        const std::wstring ntfsProbe = L"\\secmelt-writeprobe.bin";
        std::ofstream out(localProbe, std::ios::binary | std::ios::trunc);
        if (!out) {
            log.warn(Fail(L"write-path diagnosis",
                          FormatW(L"cannot create %ls", localProbe.c_str())));
            return;
        }
        std::vector<unsigned char> pattern(kProbeSize, kProbeByte);
        out.write(reinterpret_cast<const char*>(pattern.data()),
                  static_cast<std::streamsize>(pattern.size()));
        out.close();

        int exitCode = -1;
        std::wstring output;
        std::wstring error;
        bool wrote = false;
        if (NtfsCopyIn(device.raw(), vol, localProbe, ntfsProbe, exitCode, output, error) &&
            exitCode == 0) {
            wrote = true;
        } else {
            log.warn(Fail(L"write-path diagnosis: the pattern probe write failed",
                          error.empty() ? output : error));
        }
        if (wrote) {
            // 逐字节比对（复用下面的公共逻辑）
            const std::filesystem::path back = scratchDir / L"secmelt-writeprobe.back.bin";
            int exit2 = -1;
            std::wstring out2;
            std::wstring err2;
            uint64_t fileSize = 0;
            if (NtfsReadFile(device.raw(), vol, ntfsProbe, back, kProbeSize, exit2, out2, err2,
                             &fileSize) &&
                exit2 == 0) {
                std::ifstream in(back, std::ios::binary | std::ios::ate);
                size_t diff = 0, zero = 0, total = 0;
                if (in) {
                    const std::streamoff n = in.tellg();
                    total = static_cast<size_t>(n > 0 ? n : 0);
                    std::vector<unsigned char> got(total);
                    in.seekg(0);
                    if (total && in.read(reinterpret_cast<char*>(got.data()),
                                         static_cast<std::streamsize>(got.size()))) {
                        for (size_t i = 0; i < total; ++i) {
                            if (got[i] != kProbeByte) {
                                ++diff;
                                if (got[i] == 0) ++zero;
                            }
                        }
                    }
                }
                if (total == kProbeSize && diff == 0) {
                    log.step(L"write-path diagnosis: a 4 MiB NEW file round-trips cleanly");
                } else {
                    log.warn(FormatW(L"write-path diagnosis: a 4 MiB NEW file ALSO failed "
                                     L"(%zu of %zu bytes differ, %zu of them 0x00, read back %zu "
                                     L"bytes)",
                                     diff, kProbeSize, zero, total));
                }
                secmelt::RemoveFile(back);
            } else {
                log.warn(Fail(L"write-path diagnosis: reading the pattern probe back failed",
                              err2.empty() ? out2 : err2));
            }
            std::wstring rmOut;
            if (!NtfsDeleteFile(device.raw(), vol, ntfsProbe, rmOut)) {
                log.warn(Fail(L"write-path diagnosis: could not remove the pattern probe",
                              FormatW(L"%ls -- delete %ls manually", rmOut.c_str(),
                                      ntfsProbe.c_str())));
            }
        }
        secmelt::RemoveFile(localProbe);
    }

    // ---- 第二组：**同一份导出、同样大小**、只是写到一个新名字 -------------------
    //
    // 这一组才是关键。第一组用的是 4 MiB 的小文件，与 hive 差着"大小/耗时"，分不清是
    // 大小问题还是"内核正持有那个文件"的问题。这一组把**完全相同的字节**（11.6 MiB 的导出）
    // 写到同目录下的新名字里 —— 内核没有打开它。于是：
    //   * 新名字**干净往返** → 大小/耗时不是原因，"内核持有那个文件"才是 → 原地重写活 hive
    //     这条路在运行中的系统上走不通；
    //   * 新名字**也脏** → 与耗时/大小有关（写入过程中被别的东西覆盖）。
    {
        /* 名字必须与分层探针（secmelt-probe.hive）区分开：preflight 的第 4 层探针目标
         * 就是 config\secmelt-probe.hive，同名会让本组的写/删把那一层的证据搅在一起
         * （第 4 层收尾 rm 报 "No such file" 就是这么来的）。 */
        const std::wstring ntfsProbe = L"\\Windows\\System32\\config\\secmelt-fullprobe.hive";
        const std::filesystem::path back = scratchDir / L"secmelt-fullprobe.back.bin";

        std::error_code ec;
        if (!secmelt::PathIsRegularFile(exportFile)) {
            log.warn(L"write-path diagnosis: the export is gone; skipping the full-size probe");
            return;
        }
        uint64_t exportSize = 0;
        {
            std::ifstream in(exportFile, std::ios::binary | std::ios::ate);
            exportSize = in ? static_cast<uint64_t>(in.tellg()) : 0;
        }
        log.step(FormatW(L"write-path diagnosis: writing the SAME %llu-byte export to a NEW name "
                         L"(%ls) -- same bytes, same size, same path, only the target differs",
                         exportSize, ntfsProbe.c_str()));

        int exitCode = -1;
        std::wstring output;
        std::wstring error;
        if (!NtfsCopyIn(device.raw(), vol, exportFile, ntfsProbe, exitCode, output, error) ||
            exitCode != 0) {
            log.warn(Fail(L"write-path diagnosis: the full-size probe write failed",
                          error.empty() ? output : error));
        } else {
            int exit2 = -1;
            std::wstring out2;
            std::wstring err2;
            uint64_t fileSize = 0;
            if (NtfsReadFile(device.raw(), vol, ntfsProbe, back, exportSize, exit2, out2, err2,
                             &fileSize) &&
                exit2 == 0) {
                std::ifstream exp(exportFile, std::ios::binary | std::ios::ate);
                std::ifstream in(back, std::ios::binary | std::ios::ate);
                if (exp && in) {
                    const std::streamoff en = exp.tellg();
                    const std::streamoff gn = in.tellg();
                    std::vector<unsigned char> want(static_cast<size_t>(en > 0 ? en : 0));
                    std::vector<unsigned char> got(static_cast<size_t>(gn > 0 ? gn : 0));
                    exp.seekg(0);
                    in.seekg(0);
                    bool readOk = true;
                    if (!want.empty() &&
                        !exp.read(reinterpret_cast<char*>(want.data()),
                                  static_cast<std::streamsize>(want.size()))) {
                        readOk = false;
                    }
                    if (!got.empty() &&
                        !in.read(reinterpret_cast<char*>(got.data()),
                                 static_cast<std::streamsize>(got.size()))) {
                        readOk = false;
                    }
                    if (readOk) {
                        const size_t common =
                            want.size() < got.size() ? want.size() : got.size();
                        size_t diff = 0, zero = 0, first = 0;
                        bool seen = false;
                        for (size_t i = 0; i < common; ++i) {
                            if (want[i] != got[i]) {
                                if (!seen) {
                                    seen = true;
                                    first = i;
                                }
                                ++diff;
                                if (got[i] == 0) ++zero;
                            }
                        }
                        if (want.size() != got.size()) seen = true;
                        if (!seen) {
                            log.warn(L"write-path verdict: the SAME bytes at the SAME size written "
                                     L"to a NEW name round-trip PERFECTLY -- so size and duration "
                                     L"are not the problem. The hive fails because the running "
                                     L"kernel holds that exact file open: its cached metadata/data "
                                     L"for it gets written back over our raw changes. Rewriting a "
                                     L"live hive in place is not viable on a running system");
                        } else {
                            log.warn(FormatW(L"write-path verdict: the SAME bytes written to a NEW "
                                             L"name ALSO fail (%zu of %zu bytes differ, first at "
                                             L"%zu, %zu of them 0x00; read back %zu vs wrote %zu) "
                                             L"-- so it is not about that particular file; "
                                             L"size/duration or the write itself is at fault",
                                             diff, common, first, zero, got.size(), want.size()));
                        }
                    }
                }
            } else {
                log.warn(Fail(L"write-path diagnosis: reading the full-size probe back failed",
                              err2.empty() ? out2 : err2));
            }
            std::wstring rmOut;
            if (!NtfsDeleteFile(device.raw(), vol, ntfsProbe, rmOut)) {
                log.warn(Fail(L"write-path diagnosis: could not remove the full-size probe",
                              FormatW(L"%ls -- delete %ls manually", rmOut.c_str(),
                                      ntfsProbe.c_str())));
            }
            secmelt::RemoveFile(back);
        }

    // ---- 第三组：大小二分 ------------------------------------------------------
    //
    // 4 MiB 过、11.6 MiB 不过，中间那条线在哪决定了下一步查哪里：
    //   * 阈值稳定且与文件大小无关地出现在某个 LCN/绝对偏移 → 落盘位置的问题；
    //   * 阈值稳定出现在某个大小（例如 8 MiB）→ 单次传输/缓冲区的问题；
    //   * 阈值随机游走 → 丢数据是概率性的（缓存/重试掩盖短写）。
    // 每一档都用**导出的同一段前缀**，写到各自的新名字，写完读回并把 inode 的
    // data_size / initialized_size 打出来 —— 后者是"NTFS 认为多少字节有效"的判据，
    // 它比内容比对更直接：内容相同但 initialized_size 小，就是元数据没提交。
    {
        const std::filesystem::path back = scratchDir / L"secmelt-bisect.back.bin";

        std::vector<unsigned char> exportBytes;
        {
            std::ifstream in(exportFile, std::ios::binary | std::ios::ate);
            if (in) {
                const std::streamoff n = in.tellg();
                if (n > 0) {
                    exportBytes.resize(static_cast<size_t>(n));
                    in.seekg(0);
                    if (!in.read(reinterpret_cast<char*>(exportBytes.data()), n)) exportBytes.clear();
                }
            }
        }
        if (exportBytes.empty()) {
            log.warn(L"write-path diagnosis: could not read the export for the size bisection");
            return;
        }

        /* 档位要对导出的大小自适应：固定 4..12 MiB 的档位只对 melt 的 11.6 MiB 导出成立，
         * preflight 的 1 MiB 导出会让循环**一格都不跑**且不留任何日志（之前的表现就是
         * "size bisection" 之后直接沉默）。导出小于 4 MiB 时用它的 1/4、1/2、3/4 与全长。 */
        std::vector<size_t> sizes;
        if (exportBytes.size() >= (4u << 20)) {
            for (const size_t s : {4u << 20, 6u << 20, 8u << 20, 10u << 20, 12u << 20})
                if (s < exportBytes.size()) sizes.push_back(s);
            sizes.push_back(exportBytes.size());
        } else {
            const size_t full = exportBytes.size();
            for (const size_t s : {full / 4, full / 2, (full * 3) / 4, full})
                if (s >= 4096 && (sizes.empty() || s != sizes.back())) sizes.push_back(s);
        }
        if (sizes.empty()) {
            log.warn(L"write-path diagnosis: the export is too small for a size bisection");
            return;
        }

        log.step(FormatW(L"write-path diagnosis: size bisection over %zu sizes (max %zu bytes, "
                         L"same export prefix, fresh name each time)",
                         sizes.size(), sizes.back()));
        for (const size_t want : sizes) {
            const std::filesystem::path localPrefix = scratchDir / L"secmelt-bisect.bin";
            std::ofstream out(localPrefix, std::ios::binary | std::ios::trunc);
            if (!out) break;
            out.write(reinterpret_cast<const char*>(exportBytes.data()),
                      static_cast<std::streamsize>(want));
            out.close();

            const std::wstring ntfsProbe = L"\\Windows\\System32\\config\\secmelt-bisect.hive";
            int exitCode = -1;
            std::wstring output;
            std::wstring error;
            if (!NtfsCopyIn(device.raw(), vol, localPrefix, ntfsProbe, exitCode, output, error) ||
                exitCode != 0) {
                log.warn(FormatW(L"  %7zu bytes: ntfscp FAILED (exit %d) -- %ls", want, exitCode,
                                 (error.empty() ? output : error).c_str()));
            } else {
                int exit2 = -1;
                std::wstring out2;
                std::wstring err2;
                uint64_t fileSize = 0;
                size_t diff = 0, zero = 0, first = 0;
                bool seen = false;
                size_t total = 0;
                if (NtfsReadFile(device.raw(), vol, ntfsProbe, back, want, exit2, out2, err2,
                                 &fileSize) &&
                    exit2 == 0) {
                    std::ifstream in(back, std::ios::binary | std::ios::ate);
                    if (in) {
                        const std::streamoff n = in.tellg();
                        total = static_cast<size_t>(n > 0 ? n : 0);
                        std::vector<unsigned char> got(total);
                        in.seekg(0);
                        if (total && in.read(reinterpret_cast<char*>(got.data()),
                                             static_cast<std::streamsize>(total))) {
                            const size_t common = want < total ? want : total;
                            for (size_t i = 0; i < common; ++i) {
                                if (exportBytes[i] != got[i]) {
                                    if (!seen) {
                                        seen = true;
                                        first = i;
                                    }
                                    ++diff;
                                    if (got[i] == 0) ++zero;
                                }
                            }
                        }
                    }
                }
                std::wstring info;
                NtfsAttrInfo(device.raw(), vol, ntfsProbe, info);
                std::wstring meta;
                {
                    // 只留 data_size / initialized_size 两个字段，日志一行放得下
                    const size_t p = info.find(L"data_size=");
                    if (p != std::wstring::npos) {
                        const size_t q = info.find(L"compressed_size=");
                        meta = info.substr(p, (q == std::wstring::npos ? info.size() : q) - p);
                    }
                }
                for (auto& c : meta) {
                    if (c == L'\n' || c == L'\r') c = L' ';
                }
                log.step(FormatW(L"  %7zu bytes: read back %zu, %zu byte(s) differ (first at %zu, "
                                 L"%zu zero) | %ls",
                                 want, total, diff, first, zero, meta.c_str()));
                // MFT 记录法证倾倒（record:）—— initialized_size 出现"任何合法写入者
                // 都产生不了的值"时（如 4701），属性布局、记录序号与
                // $STANDARD_INFORMATION 时间戳就是"最后写入者"的笔迹。每档一条 step
                // 行，避免上一行的 meta 截断逻辑把它丢掉。
                {
                    std::wstring rec;
                    if (NtfsRecordDump(device.raw(), vol, ntfsProbe, rec) && !rec.empty()) {
                        log.step(L"  record forensics (" + std::to_wstring(want) + L"): " + rec);
                    }
                }
            }
            std::wstring rmOut;
            NtfsDeleteFile(device.raw(), vol, ntfsProbe, rmOut);
            secmelt::RemoveFile(localPrefix);
            secmelt::RemoveFile(back);
        }
    }
    }
}

// 写回前的 $LogFile 状态守卫：RW 挂载（ntfscp -f）在重启页 v2.0 时被 libntfs-3g
// **无条件拒绝**（"Windows 持有缓存元数据"：fast startup / 休眠 / 掉电后的状态，
// volume.c 对 EPERM 连 NTFS_MNT_RECOVER 都绕不过）。与其让 ntfscp 在 preflight
// 里摔一个 exit 1，不如把状态在动手之前显式报出来并指路。
// 只读挂载，一个字节都不写。
bool LogfileStateGuard(WinDiskDevice& device, const VolumeInfo& vol, const Log& log) {
    std::wstring state;
    if (!NtfsLogState(device.raw(), vol, state)) {
        log.warn(Fail(L"pre-write guard (logfile state)", state));
        return false;
    }
    log.step(L"logfile state:\n" + state);
    const bool cached = state.find(L"cached_metadata=YES") != std::wstring::npos;
    if (cached) {
        log.warn(Fail(L"pre-write guard (logfile state)",
                      L"$LogFile restart page is v2.0 -- Windows holds cached metadata for "
                      L"this volume (fast startup / hibernation / power-cut state). "
                      L"ntfscp cannot mount it read-write and neither should we write the "
                      L"hive while the OS cache can overwrite it. Do a REAL shutdown "
                      L"(shutdown /s, not restart) or 'powercfg /h off', then retry"));
        return false;
    }
    return true;
}

// 写回之后的读回校验。返回 true = "ntfs-3g 读到的那份就是我们写下去的那份，且它的 base block
// 干净、校验和有效"。
// 为什么用 ntfs-3g 读回来比对，而不是按运行中文件系统的 $MFT 取物理区段、再经驱动读：
//   * 写是 ntfscp 按 **ntfs-3g 的视图**做的，校验就该用同一个视图；
//   * 运行中文件系统的视图恰恰是**可疑的一方**（卷被还原类软件接管时，它与基线布局可能不一致）。
//     拿可疑的一方去验证被怀疑的操作，逻辑上不成立 —— 这一点在写前守卫上已经吃过一次。
//   * 顺带绕开一个硬约束：活动 SYSTEM hive 被内核独占持有，按 Win32 路径根本打不开
//     （ERROR_SHARING_VIOLATION=32）。
//
// 这个断言是整条链路的地基：内核只在 hive「脏」时才做日志恢复；干净的 hive **忽略**日志里的
// 后续日志项（regf 规范「Dirty state of a hive」）。所以只要读回来的是干净且校验和有效的
// hive，我们写下去的改动就不可能被日志重放覆盖回去。
bool VerifyHiveWrite(WinDiskDevice& device, const VolumeInfo& vol, const std::wstring& ntfsPath,
                     const std::filesystem::path& localFile,
                     const std::filesystem::path& scratchFile, const Log& log) {
    std::wstring error;

    // 本地导出 → 长度 + 内容（期望值）
    std::ifstream in(localFile, std::ios::binary | std::ios::ate);
    if (!in) {
        log.warn(Fail(L"read back the written hive",
                      FormatW(L"cannot open %ls", localFile.c_str())));
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        log.warn(Fail(L"read back the written hive", FormatW(L"%ls is empty", localFile.c_str())));
        return false;
    }
    std::vector<unsigned char> expected(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(expected.data()),
                 static_cast<std::streamsize>(expected.size()))) {
        log.warn(Fail(L"read back the written hive",
                      FormatW(L"cannot read %ls", localFile.c_str())));
        return false;
    }

    // ntfs-3g → 磁盘上那份的内容
    int exitCode = -1;
    std::wstring output;
    if (!NtfsReadFile(device.raw(), vol, ntfsPath, scratchFile, expected.size(), exitCode, output,
                      error) ||
        exitCode != 0) {
        log.warn(Fail(L"read back the written hive",
                      error.empty() ? output : error));
        return false;
    }

    std::ifstream back(scratchFile, std::ios::binary | std::ios::ate);
    if (!back) {
        log.warn(Fail(L"read back the written hive",
                      FormatW(L"cannot open %ls", scratchFile.c_str())));
        return false;
    }
    const std::streamoff backSize = back.tellg();
    std::vector<unsigned char> actual(static_cast<size_t>(backSize > 0 ? backSize : 0));
    back.seekg(0);
    if (!actual.empty() &&
        !back.read(reinterpret_cast<char*>(actual.data()),
                   static_cast<std::streamsize>(actual.size()))) {
        log.warn(Fail(L"read back the written hive", L"cannot read the read-back file"));
        return false;
    }
    log.step(FormatW(L"hive read-back via ntfs-3g: %zu of %zu bytes",
                     actual.size(), expected.size()));

    // 逐字节比对（含长度）：不只报第一个差异，而是给出**差异统计** —— 这直接区分
    // "丢了一个字节"、"整片区域是零"、"整段是旧内容"这几种完全不同的成因。
    size_t diffCount = 0;
    size_t firstDiff = 0;
    size_t lastDiff = 0;
    size_t diskZeroCount = 0;
    bool mismatch = false;
    const size_t common = expected.size() < actual.size() ? expected.size() : actual.size();
    for (size_t i = 0; i < common; ++i) {
        if (actual[i] != expected[i]) {
            if (!mismatch) {
                mismatch = true;
                firstDiff = i;
            }
            lastDiff = i;
            ++diffCount;
            if (actual[i] == 0) ++diskZeroCount;
        }
    }
    if (expected.size() != actual.size()) mismatch = true;
    if (mismatch) {
        // 决定性失败：ntfs-3g 读到的不是我们写下去的内容。
        log.warn(FormatW(L"the hive ntfs-3g reads back differs from the export: %zu differing byte(s) "
                         L"out of %zu; first at %zu (disk 0x%02X, file 0x%02X), last at %zu; "
                         L"%zu of them are 0x00 on disk; disk length %zu vs export %zu",
                         diffCount, common, firstDiff,
                         common ? actual[firstDiff] : 0, common ? expected[firstDiff] : 0, lastDiff,
                         diskZeroCount, actual.size(), expected.size()));

        // 再读一次同样的大小做对照，用来区分两种完全不同的成因：
        //   * 两次读回**一致** → 卷是稳定的，差异是"写就没写全"（ntfscp/驱动/连接层）；
        //   * 两次读回**不同** → 有东西正在改这个卷（还原类软件的驱动在把块改回去）。
        const std::filesystem::path secondScratch = scratchFile.wstring() + L".2";
        int exit2 = -1;
        std::wstring output2;
        std::wstring error2;
        if (NtfsReadFile(device.raw(), vol, ntfsPath, secondScratch, expected.size(), exit2, output2,
                         error2) &&
            exit2 == 0) {
            std::ifstream again(secondScratch, std::ios::binary | std::ios::ate);
            if (again) {
                const std::streamoff n2 = again.tellg();
                std::vector<unsigned char> actual2(static_cast<size_t>(n2 > 0 ? n2 : 0));
                again.seekg(0);
                if (!actual2.empty() &&
                    again.read(reinterpret_cast<char*>(actual2.data()),
                               static_cast<std::streamsize>(actual2.size()))) {
                    const size_t c2 = actual.size() < actual2.size() ? actual.size() : actual2.size();
                    size_t drift = 0, driftFirst = 0;
                    bool driftSeen = false;
                    for (size_t i = 0; i < c2; ++i) {
                        if (actual[i] != actual2[i]) {
                            if (!driftSeen) {
                                driftSeen = true;
                                driftFirst = i;
                            }
                            ++drift;
                        }
                    }
                    if (actual.size() != actual2.size()) driftSeen = true;
                    if (driftSeen) {
                        log.warn(FormatW(L"the volume is CHANGING between reads: %zu byte(s) differ "
                                         L"between two consecutive read-backs of the same file "
                                         L"(first at %zu) -- something is rewriting this volume "
                                         L"while we work",
                                         drift, driftFirst));
                    } else {
                        log.warn(L"the volume is stable between two consecutive read-backs, so the "
                                 L"difference is a write that did not land in full (the tool "
                                 L"reported success but the bytes are not on disk)");
                    }
                }
            }
        }
        secmelt::RemoveFile(secondScratch);

        // 把磁盘上那个 inode 的元数据摆出来 —— 这是分辨"读回一大片零"成因的关键证据：
        // NTFS 读到 initialized_size 之外返回零，runlist 的 hole 也返回零。若这里显示
        // initialized_size < data_size 或存在 hole，那么"读回不一致"就不是"写没落上"，
        // 而是"落到了一个按 NTFS 语义读出来是零的位置"。
        {
            std::wstring info;
            if (NtfsAttrInfo(device.raw(), vol, ntfsPath, info)) {
                log.warn(L"on-disk inode metadata for the target:\n" + info);
            } else {
                log.warn(Fail(L"read the target's inode metadata", info));
            }
        }
        // MFT 记录的原始字节/布局（record:）—— initialized_size 出现任何合法写入者
        // 都产生不了的值时，属性布局、序号与 $STANDARD_INFORMATION 时间戳就是
        // "最后写入者"的笔迹，这是定案证据。
        {
            std::wstring rec;
            if (NtfsRecordDump(device.raw(), vol, ntfsPath, rec)) {
                log.warn(L"MFT record forensics for the target:\n" + rec);
            }
        }

        // 卷稳定时再往下挖一层：写路径本身行不行？（同卷、同路径、临时文件往返）
        DiagnoseWritePath(device, vol, localFile, localFile.parent_path(), log);

        return false;
    }

    // 结构性关卡只对**内容确实是 hive**的写回生效：--preflight 的分层探针在本地没有
    // 导出文件时是合成字节（(i*31+17)&0xFF 的重复模式，根本不是 regf）——对它要求
    // base block 有效等于把"写通道完全没问题"误报成失败。melt 写回的导出总是 regf，
    // 该走的检查一步不少。
    {
        HiveBaseBlock expectedBlock;
        const bool contentIsHive =
            ParseBaseBlock(expected.data(), expected.size(), expected.size(), expectedBlock) &&
            expectedBlock.signatureOk;
        if (!contentIsHive) {
            log.ok(L"ntfs-3g reads back the exact bytes we wrote; the probe content is not a "
                   L"regf hive, so structure-level checks do not apply to it");
            return true;
        }
    }

    // base block 直接用读回来的那份解析 —— 它才是下次启动时内核会看到的东西。
    HiveBaseBlock onDisk;
    if (!ParseBaseBlock(actual.data(), actual.size(), actual.size(), onDisk)) {
        log.warn(L"the base block read back is not parseable");
        return false;
    }
    log.step(L"base block on disk: " + DescribeBaseBlock(onDisk));
    const bool baseOk = onDisk.signatureOk && onDisk.clean && onDisk.checksumOk;
    if (!baseOk) {
        log.warn(L"the base block on disk is not a clean, checksum-valid hive: the kernel would "
                 L"treat it as dirty and try to replay the logs");
        return false;
    }
    log.ok(L"ntfs-3g reads back the exact hive we wrote, and its base block is clean and "
           L"checksum-valid");
    return true;
}

}  // namespace

// 确保「未签名的 WinDisk 能被装载」：装载 → 被 577 拒绝则 kdu -dse 0 → 重新装载 → 复测。
// melt 的第一步与环境自检共用这一份实现（环境自检原先只报"melt 会去关它"，那是预测不是结论）。
//
// 服务键的清理只发生在"确定没有驱动在跑"时：装载成功就**绝不能删**（驱动还加载着，删键会让
// SCM 与注册表脱节 —— 实测后果是下一次 ChangeServiceConfigW 报 error 2）。
DriverLoadOutcome EnsureUnsignedDriverLoads(const std::filesystem::path& exeDir, bool allowKdu) {
    DriverLoadOutcome outcome;
    const std::filesystem::path sysPath = exeDir / kDefaultSysName;
    if (!secmelt::PathIsRegularFile(sysPath)) {
        outcome.firstError = FormatW(L"driver file not found: %ls", sysPath.c_str());
        return outcome;
    }

    std::wstring error;
    outcome.result = LoadDriver(sysPath, kServiceName, error);
    if (outcome.result == DriverLoad::Loaded) {
        // WinDisk 已经在内核中运行，不能在这里卸载；RunMelt 会在所有 raw 操作完成后
        // 先关闭设备再删除 SCM 服务键。导出的 SYSTEM 副本也会单独剥离该键。
        return outcome;
    }
    outcome.firstError = error;

    // 到这里的两种失败都没有驱动在跑（被拒的装载不会加载任何东西），所以删掉本次探测
    // 建出来的服务键是安全的 —— 不删的话，每次提权探测都会在活动注册表里留一个。
    const auto cleanup = [&] { secmelt::UnloadDriver(kServiceName); };
    if (outcome.result != DriverLoad::SignatureRejected) {
        cleanup();  // 与签名无关的失败（服务注册/权限/文件…）
        return outcome;
    }
    if (!allowKdu) {
        cleanup();  // 纯报告路径（--dump）：如实说"被拦"，不去改系统
        return outcome;
    }

    // 577 = 签名强制还在拦，这是唯一的"DSE 没解开"证据。关掉它，再用重新装载来证明真的解开了。
    outcome.kduRan = true;
    {
        // 只要 sink：把 kdu 的原始输出收进 outcome，由调用方决定怎么展示
        const Log collect{nullptr, [&](const std::wstring& line) {
                              outcome.kduLog += line + L"\n";
                          }};
        std::wstring kduError;
        if (!RunKduDseOff(exeDir, collect, kduError)) {
            outcome.retryError = kduError;
            cleanup();
            return outcome;
        }
    }

    outcome.result = LoadDriver(sysPath, kServiceName, error);
    if (outcome.result != DriverLoad::Loaded) {
        outcome.retryError = error;
        cleanup();
    }
    return outcome;
}

MeltResult RunMelt(const MeltOptions& opt, const MeltLogger& log) {
    MeltResult result;
    const Log emit{&result, log};
    const std::filesystem::path exeDir = opt.exeDir.empty() ? ExeDir() : opt.exeDir;
    std::wstring error;
    std::error_code ec;

    emit.step(FormatW(L"SecMelt melt starting (%ls mode)", opt.dryRun ? L"dry-run" : L"apply"));

    // ---- 1. 预检 ---------------------------------------------------------------
    if (!IsProcessElevated()) {
        emit.warn(Fail(L"preflight", L"process is not elevated"));
        return result;
    }

    // DSE（驱动签名强制）不看任何"查询"接口 —— 判据是装载驱动的返回码：未签名的
    // WinDisk 只有 DSE 真被解开才装得上（577 = ERROR_INVALID_IMAGE_HASH = 还在拦）。
    // 这比 NtQuerySystemInformation 的 CodeIntegrityOptions 更贴近事实：那一项只反映 CI
    // 标志位，而 WDAC 策略、易受攻击驱动黑名单同样会让装载返回 577。
    // 第 7 步的确认清单要列出这一步，而 dry-run 时还不知道需不需要（要真去装才知道），
    // 所以把措辞记在这里。
    std::wstring dsePending;

    const std::filesystem::path sysPath =
        opt.winDiskSysPath.empty() ? (exeDir / kDefaultSysName) : opt.winDiskSysPath;
    if (!secmelt::PathIsRegularFile(sysPath)) {
        emit.warn(Fail(L"preflight / WinDisk driver", FormatW(L"missing: %ls", sysPath.c_str())));
        if (!opt.dryRun) return result;
    } else {
        emit.ok(FormatW(L"WinDisk driver: %ls", sysPath.c_str()));
    }

    // ---- 2/3. 装载驱动、定位系统卷 --------------------------------------------
    // 顺序有意如此：先确认裸盘通路可用，再去动注册表。驱动装不上就必须停在
    // 「注册表还没被改」的状态，否则会留下一个过滤器已摘、hive 却没写回的系统。
    WinDiskDevice device;
    VolumeInfo vol;
    if (opt.dryRun) {
        dsePending = L"disable driver signature enforcement (kdu -dse 0) if the driver load is "
                     L"rejected with 577 (ERROR_INVALID_IMAGE_HASH)";
        emit.step(L"dry-run: the driver is not loaded; whether DSE must be turned off is only "
                  L"decided by an actual load attempt (577 = still enforced)");
    } else {
        // 装载 + 必要时 kdu -dse 0 + 重新装载复测（与 TUI 的环境自检共用一份实现）
        const DriverLoadOutcome load = EnsureUnsignedDriverLoads(exeDir, /*allowKdu=*/true);
        if (!load.firstError.empty()) emit.warn(Fail(L"load driver", load.firstError));
        if (load.kduRan) {
            emit.warn(L"DSE is still enforced; disabling it via KDU and retrying the load");
            // kdu 的输出逐行进日志（它的诊断信息对判断失败原因有用）
            size_t begin = 0;
            while (begin < load.kduLog.size()) {
                const size_t nl = load.kduLog.find(L'\n', begin);
                const std::wstring one(load.kduLog, begin,
                                       nl == std::wstring::npos ? std::wstring::npos : nl - begin);
                if (!one.empty()) emit.step(one);
                if (nl == std::wstring::npos) break;
                begin = nl + 1;
            }
        }
        if (load.result == DriverLoad::Loaded) {
            if (load.kduRan) {
                dsePending = L"disable driver signature enforcement (kdu -dse 0)";
                emit.ok(L"DSE was enforced; the driver now loads after kdu -dse 0");
            } else {
                emit.ok(L"driver loaded without touching DSE (the load was not rejected)");
            }
        } else {
            emit.warn(Fail(L"load driver (after the DSE step)", load.retryError.empty()
                                                                   ? load.firstError
                                                                   : load.retryError));
            if (load.result == DriverLoad::SignatureRejected) {
                emit.warn(Fail(L"disable DSE",
                               L"the load is still rejected -- what blocks it is not the CI flags "
                               L"kdu patches (HVCI / memory integrity on, a WDAC policy, or the "
                               L"Microsoft vulnerable driver blocklist)"));
            }
            return result;
        }

        if (!device.Open(error)) {
            emit.warn(Fail(L"open device", error));
            return result;
        }
        emit.ok(FormatW(L"opened device %ls", DeviceSymbolicLink()));

        if (!QueryVolumeInfo(kSystemVolume, vol, error)) {
            emit.warn(Fail(L"query volume info", error));
            return result;
        }
        emit.ok(FormatW(L"volume %ls: disk=%u offset=%llu length=%llu (bps=%u spc=%u)", kSystemVolume,
                        vol.diskNumber, vol.volumeOffset, vol.volumeLength, vol.bytesPerSector,
                        vol.sectorsPerCluster));

        if (!device.SetTargetDisk(vol.diskNumber, error)) {
            emit.warn(Fail(L"CTL_CHANGE_TARGET_DISK", error));
            return result;
        }
        emit.ok(FormatW(L"target disk set to %u", vol.diskNumber));

    }

    // ---- 4. 名单 ---------------------------------------------------------------
    std::vector<std::filesystem::path> searched;
    const auto targets = LoadTargets(exeDir, &searched);
    const auto names = TargetNames(targets);
    for (const auto& path : searched) {
        emit.step(FormatW(L"looked for targets file: %ls (%ls)", path.c_str(),
                          secmelt::PathIsRegularFile(path) ? L"present" : L"absent"));
    }
    emit.ok(FormatW(L"targets loaded: %zu entries", targets.size()));
    for (const auto& t : targets) emit.step(FormatW(L"  %ls  (%ls)", t.name.c_str(), t.label.c_str()));

    // 注册表这一步是否真的改动了东西：决定后面中止时要不要点明"已留下半成品状态"。
    bool registryModified = false;

    // 注册表被改过之后，任何中止都必须把这件事讲清楚：改动在**内存里的活动注册表**上，
    // Windows 的懒写回会在几秒内落到磁盘 —— 也就是说下次正常重启时这些过滤器驱动已经
    // 不会加载了，哪怕这次根本没写 hive。不点明的话，"nothing was written to the raw
    // disk" 会让人以为系统回到了原样。
    const auto abortAfterRegistry = [&](const std::wstring& message) {
        emit.warn(message);
        if (registryModified) {
            emit.warn(L"note: the live registry was already modified in this run (filter entries "
                      L"rewritten / service keys deleted). Windows flushes those lazily, so the "
                      L"next normal reboot already drops these filter drivers even though the "
                      L"SYSTEM hive was never rewritten - a partial state, not a no-op.");
        }
        return result;
    };

    // ---- 5. 注册表摘除（dryRun 时只读探测，不调用任何写 API）------------------
    if (opt.dryRun) {
        const ProbeReport probe = ProbeTargets(names);
        emit.step(L"registry probe (read-only; no write API is called)");
        for (const auto& f : probe.filters) {
            if (!f.present) {
                emit.step(FormatW(L"  Class\\%ls\\%ls: absent", f.classGuid.c_str(), f.valueName.c_str()));
                continue;
            }
            std::wstring items = FormatW(L"  Class\\%ls\\%ls: [", f.classGuid.c_str(), f.valueName.c_str());
            for (size_t i = 0; i < f.items.size(); ++i) items += (i ? L", " : L"") + f.items[i];
            items += L"]";
            emit.step(items);
            for (const auto& r : f.toRemove) emit.warn(FormatW(L"    would remove: %ls", r.c_str()));
        }
        for (const auto& key : probe.existingServiceKeys) {
            // **只查询**，不做控制操作 —— 这是只读预演，绝不能真的去 ControlService。
            // 报出"现在是否在运行"是因为：只删注册表键停不下已加载的驱动，
            // 所以"重启前保护仍然生效"这件事必须在预演里就看得见。
            bool running = false;
            std::wstring detail;
            std::wstring note;
            if (QueryServiceRunning(key, running, detail)) {
                // detail 自己已经写了 "RUNNING (state N)" / "not running"，不要再套一层
                note = FormatW(L" [%ls]", detail.c_str());
            } else {
                note = FormatW(L" [state unknown: %ls]", detail.c_str());
            }
            emit.warn(FormatW(L"would stop and delete service key: %ls%ls",
                              ServiceKeyPath(key).c_str(), note.c_str()));
        }
        for (const auto& key : probe.absentServiceKeys) {
            emit.step(FormatW(L"service key absent: %ls", key.c_str()));
        }
    } else {
        const EditReport filters = StripFilterEntries(names);
        for (const auto& v : filters.changedValues) emit.ok(FormatW(L"rewrote filter value: %ls", v.c_str()));
        if (filters.changedValues.empty()) emit.step(L"no filter entry matched the target list");

        const EditReport services = DeleteServiceKeys(names);
        for (const auto& s : services.stoppedServices) {
            emit.ok(FormatW(L"stopped service: %ls", s.c_str()));
        }
        for (const auto& s : services.runningServices) {
            // 不是致命错误：过滤器驱动大多没有卸载例程，重启后不加载才是效果。
            // 但必须说清"现在它还在跑"，别让人以为保护已经失效。
            emit.warn(FormatW(L"service NOT stopped: %ls -- it stays active until the next reboot",
                              s.c_str()));
        }
        for (const auto& k : services.deletedKeys) emit.ok(FormatW(L"deleted service key: %ls", k.c_str()));
        for (const auto& k : services.absentKeys) emit.step(FormatW(L"service key absent: %ls", k.c_str()));
        if (services.deletedKeys.empty()) emit.step(L"no target service key existed in the registry");

        for (const auto& f : filters.failures) emit.warn(Fail(L"filter value", f));
        for (const auto& f : services.failures) emit.warn(Fail(L"service key", f));
        // 部分成功也算"已经改过了"：写进去的那几条同样会被懒写回落到磁盘。
        registryModified = !filters.changedValues.empty() || !services.deletedKeys.empty();
        if (!filters.failures.empty() || !services.failures.empty()) {
            return abortAfterRegistry(L"registry stripping failed; aborting before any raw disk write");
        }
    }

    // ---- 6. 导出 hive，校验/修补 base block -----------------------------------
    const std::filesystem::path hivePath =
        opt.hiveOutPath.empty() ? DefaultHivePath() : opt.hiveOutPath;
    if (!ExportSystemHive(hivePath, error)) {
        return abortAfterRegistry(Fail(L"export SYSTEM hive", error));
    }
    emit.ok(FormatW(L"exported SYSTEM hive to %ls", hivePath.c_str()));

    // 把**装载用的服务键**从这份副本里摘掉（见 hive.h::RemoveServiceFromExportedHive）。
    // 这是"改副本，不改活动注册表"：活动注册表那一侧归 SCM 管（CreateServiceW /
    // ChangeServiceConfigW），我们只在写回目标机的那份 hive 里删掉它 —— 否则目标系统会永久
    // 留下一个 ImagePath 指向本机 exe 目录的 WinDisk 服务。
    //
    // dry-run 不做：它要 RegLoadKey，那会改动系统注册表状态，与"只读预演"相抵触。
    if (opt.dryRun) {
        emit.step(L"dry-run: the loader's service key is not removed from the export here (it needs "
                  L"RegLoadKey); it would be stripped before the write-back");
    } else {
        std::wstring stripError;
        if (RemoveServiceFromExportedHive(hivePath, kServiceName, stripError)) {
            emit.ok(FormatW(L"removed the transient service key '%ls' from the exported hive",
                            kServiceName));
        } else {
            return abortAfterRegistry(Fail(L"remove the loader's own service key from the exported hive",
                                            stripError));
        }
    }


    HiveBaseBlock before;
    if (!ReadBaseBlock(hivePath, before, error)) {
        return abortAfterRegistry(Fail(L"read base block", error));
    }
    emit.step(L"base block (before): " + DescribeBaseBlock(before));

    HiveBaseBlock after;
    if (!MakeCleanAndFixChecksum(hivePath, after, error)) {
        return abortAfterRegistry(Fail(L"clean / fix checksum", error));
    }
    emit.step(L"base block (after): " + DescribeBaseBlock(after));
    if (before.sequence2 != after.sequence2 || before.storedChecksum != after.storedChecksum) {
        emit.warn(L"hive was not clean/valid as saved; base block was rewritten");
    } else {
        emit.ok(L"hive was already clean with a valid checksum; nothing to fix");
    }

    if (!VerifyHiveLoadable(hivePath, error)) {
        return abortAfterRegistry(Fail(L"verify hive", error));
    }
    emit.ok(L"hive verified: signature, type, alignment, sequence numbers, checksum, root cell");

    // ---- 7. 用户确认（此前尚未发生任何裸盘写入）--------------------------------
    if (!dsePending.empty()) {
        // dry-run 时它还没执行（还不知道会不会执行）；apply 时已经在预检里做完了，
        // 列出来让确认清单完整 —— 那一步是已经发生的事实，不是待办。
        result.pending.push_back(dsePending);
    }
    // 事务日志不列入待办：它们**不会被改动**（见第 9 步：清零在干净 hive 下不改变任何结果）。
    result.pending.push_back(FormatW(
        L"remove the loader's transient service key '%ls' from the exported hive before writing it "
        L"back (otherwise the target keeps a service pointing at this machine's exe folder)",
        kServiceName));
    result.pending.push_back(
        FormatW(L"copy %ls to %ls on the raw disk (the volume's real name for it is resolved first, "
                L"so a case difference cannot make ntfscp create a second file)",
                hivePath.c_str(), kNtfsSystemHive));
    if (secmelt::PathIsRegularFile(kHiveBackupWin32)) {
        result.pending.push_back(FormatW(
            L"copy the same hive to %ls as well, so the primary/backup pair reports one generation "
            L"(startup repair compares them and calls a mismatch BadPatch)",
            kHiveBackupWin32));
    }
    result.pending.push_back(
        L"mark the system volume dirty ($Volume VOLUME_IS_DIRTY) so autochk checks it at the next "
        L"boot, exactly as chkdsk /f would");
    // 这一条不是"动作"而是必须让人看见的**前提**：写进 RegBack 之后，这个卷上不再存在上一代
    // 副本。而这类机器上系统还原通常被还原类软件禁掉（"系统还原修不动"），本机的自恢复路径
    // 是死的 —— 唯一的回退手段是虚拟机/宿主机快照。TUI 的确认框只显示 pending，所以放在这里。
    result.pending.push_back(
        L"NO ROLLBACK: the RegBack copy is overwritten too, so no earlier hive generation survives "
        L"on this volume; System Restore is typically disabled by the restore product. Take a VM "
        L"snapshot before continuing if you have not");
    result.pending.push_back(
        FormatW(L"trigger bugcheck 0x%08X via CTL_REBOOT_SYSTEM "
                L"(CRITICAL_STRUCTURE_CORRUPTION-with-custom-code; the machine resets "
                L"according to its crash settings)",
                kBugCheckCode));

    if (opt.dryRun) {
        emit.step(L"dry-run: no raw disk write, no reboot");
        result.ok = true;
        return result;
    }
    if (!opt.confirm) {
        return abortAfterRegistry(
            L"no confirmation callback provided; refusing to write the raw disk");
    }
    emit.warn(L"IRREVERSIBLE and UNRECOVERABLE on this machine: this run overwrites both the "
              L"primary SYSTEM hive and its RegBack copy, so no earlier generation survives here. "
              L"System Restore cannot undo it (the restore/freeze product usually disables it) - "
              L"the only way back is a VM snapshot taken before this point");
    for (const auto& item : result.pending) emit.warn(L"pending: " + item);
    if (!opt.confirm(result)) {
        return abortAfterRegistry(L"confirmation declined; nothing was written to the raw disk");
    }

    // ---- 8. 写回 hive ---------------------------------------------------------
    //
    // 顺序是刻意的：
    //   解析真名 → 写前守卫（读一眼目标）→ ntfsfix（尽力而为）→ 回滚点 → 预演 → 写真 SYSTEM
    //
    // 守卫必须在 ntfsfix **之前**：ntfsfix 会经同一个 handle: 写这个卷（置 dirty 标记、修
    // $MFTMirr、清 $LogFile），而探针要读的是"我们动手之前"的现场。

    // 解析出**卷上真实的名字**，后面的守卫与写入都用它。
    //
    // 为什么必须这么做：NTFS 大小写不敏感是靠 $UpCase 表实现的，该表不可用（或卷被改坏过）时
    // ntfs-3g 的查找会退化成精确大小写匹配 —— 大写 `SYSTEM` 查不到、而真实名字是小写
    // `system`。更危险的是 **ntfscp 在目标查不到时会 ntfs_new_file 新建一个**：那样就会在
    // 同目录里写出一个大小写不同的重名文件，真正的 hive 反而一个字节都没改。
    std::wstring ntfsHivePath = kNtfsSystemHive;
    {
        std::wstring canonical;
        bool ambiguous = false;
        if (NtfsResolvePath(device.raw(), vol, kNtfsSystemHive, canonical, &ambiguous)) {
            if (ambiguous) {
                // 目录里已经有"只有大小写不同"的重名文件。NTFS 按设计不允许这种状态，说明
                // 这个卷已经被写坏过（极可能就是 ntfscp 在大小写不匹配时 ntfs_new_file 造的）。
                // 此时再写哪一份都是错的：往里写可能正好覆盖引导器要用的那份。
                emit.warn(Fail(L"pre-write guard (hive name)",
                               FormatW(L"the directory holds more than one file whose name differs "
                                       L"only by case (the volume's real name resolves to %ls). "
                                       L"NTFS does not allow that by design, so this volume has "
                                       L"already been corrupted -- most likely by an earlier write "
                                       L"that created a second file instead of overwriting the "
                                       L"existing one. Refusing to write anything into it.",
                                       canonical.c_str())));
                return result;
            }
            if (canonical != kNtfsSystemHive) {
                emit.warn(FormatW(L"the hive's name on this volume is %ls, not %ls (ntfs-3g's exact "
                                  L"lookup fails; using the real name for both the guard and the "
                                  L"write)",
                                  canonical.c_str(), kNtfsSystemHive));
            } else {
                emit.step(FormatW(L"target name confirmed on the volume: %ls", canonical.c_str()));
            }
            ntfsHivePath = canonical;
        } else {
            emit.warn(L"could not resolve the hive's real name on this volume; using the literal "
                      L"path (the guard below will decide)");
        }
    }

    // 写回前的守卫：确认目标是 SYSTEM hive。**必须在任何写入之前** —— 这样守卫失败时还能
    // 干净收场（一个字节都没写），机器仍然可开机。
    //
    // 也**必须在 ntfsfix 之前**：ntfsfix 会经同一个 handle: 写这个卷（置 dirty 标记、修
    // $MFTMirr、清 $LogFile），而探针要读的是"我们动手之前"的现场；先让它跑再去读，读到的是
    // 它改过的状态（这也让"读不到"更难判断）。
    const std::filesystem::path guardScratch = hivePath.parent_path() / L"secmelt-guard-head.bin";
    if (!TargetHoldsHive(device, vol, ntfsHivePath, guardScratch, emit)) {
        emit.warn(L"aborting before any raw disk write: the target is not the SYSTEM hive this "
                  L"volume's file system presents");
        return result;
    }

    // ntfsfix 是"尽力而为"，**退出码非 0 不中止**：在卷的文件系统填满整个分区时它必然返回 1。
    //
    // 守卫之后才跑，理由见上。它仍然值得跑：卷挂载失败时它会修 $MFTMirr、清空 $LogFile，
    // 再把卷交回挂载 —— 那正是"裸写之前先让 NTFS 自洽"的意义。
    //
    // 只跑一次：它曾经被重复贴成两块（日志里能看到两遍输出）—— 那是把守卫挪到前面时插入了
    // 一份而不是移动造成的。
    if (opt.runNtfsFix) {
        if (ResolveTool(L"ntfsfix.exe").empty()) {
            emit.warn(Fail(L"ntfsfix", L"ntfsfix.exe not found; continuing without it"));
        } else {
            int exitCode = -1;
            std::wstring output;
            if (!NtfsFix(device.raw(), vol, exitCode, output, error)) {
                emit.warn(Fail(L"ntfsfix", error + L" (continuing without it)"));
            } else {
                emit.step(L"ntfsfix output:\n" + output);
                if (exitCode != 0) {
                    emit.warn(FormatW(
                        L"ntfsfix exited with %d -- expected here, not fatal: its "
                        L"alternate-boot-sector check only passes when the file system is smaller "
                        L"than the partition, and the handle: length is derived from the boot "
                        L"sector itself. The hive write below is unaffected.",
                        exitCode));
                } else {
                    emit.ok(L"ntfsfix reported success");
                }
            }
        }
    }

    // ---- 8. 写回 hive —— 但**先保证写坏了也能收场** ---------------------------

    // 1) 回滚点：把当前的 SYSTEM 整份读下来
    const std::filesystem::path backupPath = hivePath.parent_path() / L"secmelt-system.before.hive";
    uint64_t currentSize = 0;
    std::wstring backupError;
    bool haveBackup = false;
    {
        int exitCode = -1;
        std::wstring out;
        std::wstring readError;
        // 先取长度（readhead 会报 file size），再整份读
        if (NtfsReadFile(device.raw(), vol, ntfsHivePath, backupPath, kHiveBlockSize, exitCode, out,
                         readError, &currentSize) &&
            currentSize > 0) {
            int exit2 = -1;
            std::wstring out2;
            if (NtfsReadFile(device.raw(), vol, ntfsHivePath, backupPath, currentSize, exit2, out2,
                             readError) &&
                exit2 == 0) {
                haveBackup = true;
                emit.ok(FormatW(L"captured a rollback point: the current %llu-byte SYSTEM",
                                currentSize));
            }
        }
        if (!haveBackup) {
            // 读不到回滚点也**不中止**：读路径的问题不该阻止"把 patch 写到基线"这个任务。
            // 只如实说清"这次没有回滚点可用"。
            emit.warn(Fail(L"capture a rollback point", readError));
            emit.warn(L"continuing without a rollback image: the goal only depends on getting the "
                      L"patched hive onto the baseline, and a frozen volume redirects Windows' own "
                      L"writes so they cannot overwrite it");
        } else {
            emit.step(FormatW(L"rollback image kept at %ls (a copy taken before writing)",
                              backupPath.c_str()));
        }
    }

    // 拆掉回滚用的辅助函数：写回一份本地文件到目标并校验
    const auto writeAndVerify = [&](const std::filesystem::path& src,
                                    const std::wstring& dest) -> bool {
        int exitCode = -1;
        std::wstring output;
        std::wstring writeError;
        if (!NtfsCopyIn(device.raw(), vol, src, dest, exitCode, output, writeError)) {
            emit.warn(Fail(FormatW(L"ntfscp %ls", dest.c_str()), writeError));
            return false;
        }
        if (exitCode != 0 || output.find(L"ERROR") != std::wstring::npos) {
            emit.warn(FormatW(L"ntfscp %ls exited with %d", dest.c_str(), exitCode));
            return false;
        }
        return VerifyHiveWrite(device, vol, dest, src, guardScratch, emit);
    };

    // 1b) $LogFile 状态守卫：v2.0 重启页 = "Windows 持有缓存元数据"，ntfscp 的 RW
    // 挂载会被 libntfs-3g 无条件拒绝（连 -f 都不行）。在预演前把状态显式报出来，
    // 指路"真关机后重试"，而不是让 ntfscp 摔一个 exit 1。
    if (!LogfileStateGuard(device, vol, emit)) {
        emit.warn(L"aborting before any raw disk write: the volume's $LogFile indicates Windows "
                  "holds cached metadata; do a real shutdown and retry");
        return result;
    }

    // 2) 预演：把同一份字节写到临时名字并校验。不碰 SYSTEM 一个字节。
    const std::wstring ntfsPreflight = L"\\Windows\\System32\\config\\secmelt-preflight.hive";
    {
        uint64_t exportSize = 0;
        {
            std::ifstream in(hivePath, std::ios::binary | std::ios::ate);
            exportSize = in ? static_cast<uint64_t>(in.tellg()) : 0;
        }
        emit.step(FormatW(L"preflight: writing the same %llu bytes to a scratch name (%ls) and "
                          L"reading them back, BEFORE touching SYSTEM",
                          exportSize, ntfsPreflight.c_str()));
        const bool preflightOk = writeAndVerify(hivePath, ntfsPreflight);
        {
            std::wstring rmOut;
            if (!NtfsDeleteFile(device.raw(), vol, ntfsPreflight, rmOut)) {
                emit.warn(Fail(L"preflight cleanup",
                               FormatW(L"%ls -- delete %ls manually", rmOut.c_str(),
                                       ntfsPreflight.c_str())));
            }
        }
        if (!preflightOk) {
            // 同一份字节写到新文件都无法完整往返，不能再碰活动 SYSTEM。
            // 此时 raw 设备已经打开且 WinDisk 服务已成功加载；必须先关闭句柄再卸载并删除
            // 临时服务，否则下一次启动会再次尝试加载未签名的 WinDisk.sys 并弹出签名提示。
            emit.warn(L"preflight failed; refusing to write SYSTEM or reboot");
            result.wroteDisk = false;
            device.Close();
            if (!UnloadDriver(kServiceName)) {
                emit.warn(L"could not remove the transient WinDisk service after preflight failure");
            }
            return result;
        }
        emit.ok(L"preflight passed: the same bytes round-trip cleanly through this path");
    }

    // 3) 才写真 SYSTEM。写 + 校验最多两轮（重写同内容是幂等的，无害）。
    bool writeVerified = false;
    for (int attempt = 1; attempt <= 2 && !writeVerified; ++attempt) {
        emit.step(FormatW(L"writing SYSTEM back (attempt %d)", attempt));
        writeVerified = writeAndVerify(hivePath, ntfsHivePath);
        if (!writeVerified && attempt == 1) {
            emit.warn(L"the read-back does not match the export; rewriting once (same content, so a "
                      L"retry is harmless)");
        }
    }

    // 读回失败是硬失败：日志已经证明 ntfscp 可能吞掉短写，继续写 RegBack 或复位会把
    // 一个未验证的 SYSTEM hive 交给 winload/内核。此处必须在任何后续 raw 写入、dirty
    // 标记和 bugcheck 之前返回。
    if (!writeVerified) {
        result.wroteDisk = true;
        emit.warn(L"SYSTEM read-back verification failed; refusing to write RegBack, mark the "
                  L"volume dirty, or reset the machine");
        emit.warn(FormatW(L"rollback image is available at %ls; restore it before rebooting",
                          backupPath.c_str()));
        return result;
    }
    result.wroteDisk = true;
    emit.ok(L"SYSTEM verified: the bytes on disk are the export we intended to write");

    // ---- 8b. 保持 hive 备份副本同步 -------------------------------------------
    //
    // Windows 在 config\\RegBack\\ 下留着一份 SYSTEM 备份（Win7 有；Win10 1803 起默认清空），
    // 启动修复做"修复"时会拿它比对、甚至拿它还原。只改主 hive、把备份留在旧版本，恰恰就是
    // "注册表在离线状态下被改过一半"的样子。因此存在备份时必须把同一份导出写入并验证。
    if (secmelt::PathIsRegularFile(kHiveBackupWin32)) {
        if (!TargetHoldsHive(device, vol, kNtfsHiveBackup, guardScratch, emit)) {
            emit.warn(L"RegBack target is not a readable hive; refusing to reboot");
            return result;
        }
        if (!writeAndVerify(hivePath, kNtfsHiveBackup)) {
            result.wroteDisk = true;
            emit.warn(L"RegBack read-back verification failed; refusing to mark the volume dirty or reset");
            emit.warn(FormatW(L"rollback image is available at %ls; restore it before rebooting",
                              backupPath.c_str()));
            return result;
        }
        emit.ok(FormatW(L"%ls carries the same hive (primary and backup in sync)",
                        kHiveBackupWin32));
    } else {
        emit.step(L"no config\\RegBack\\SYSTEM on this system; nothing to keep in sync");
    }

    // ---- 9. 事务日志：只读、**不动** -------------------------------------------
    // 换句话说清零只多出三次针对启动路径文件的破坏性离线写入，换不来任何保障；而"离线改动过
    // SYSTEM 相关文件"正是启动修复判 BadPatch（"A patch is preventing the system from
    // starting"，修复动作通常是系统还原）时盯着的痕迹 —— 少动一个文件就少一分风险。
    // 真正的地基是上一步的读回校验，不是清零。这里只把现场读出来记进日志。
    for (const wchar_t* logFile : kLogFiles) {
        const std::filesystem::path path(logFile);
        if (!secmelt::PathIsRegularFile(path)) {
            emit.step(FormatW(L"%ls: absent", logFile));
            continue;
        }
        int exitCode = -1;
        std::wstring out;
        std::wstring logError;
        if (!NtfsReadFile(device.raw(), vol, logFile, guardScratch, 4, exitCode, out,
                          logError) ||
            exitCode != 0) {
            emit.warn(Fail(FormatW(L"inspect %ls", logFile), logError.empty() ? out : logError));
            continue;
        }
        std::ifstream in(guardScratch, std::ios::binary);
        unsigned char magic[4] = {};
        const bool readOk =
            in.read(reinterpret_cast<char*>(magic), 4) && in.gcount() == 4;
        if (!readOk) {
            emit.warn(Fail(FormatW(L"inspect %ls", logFile), L"readhead returned fewer than 4 bytes"));
            continue;
        }
        if (std::memcmp(magic, "regf", 4) == 0) {
            // "regf" = 它确有 base block 副本，是一份正常的日志。原样留着：
            // hive 干净时它不被采用，留着不占任何风险。
            emit.step(FormatW(L"%ls: a normal regf log, left untouched (the written hive is clean, "
                              L"so the kernel ignores log entries anyway)",
                              logFile));
        } else {
            emit.step(FormatW(L"%ls: not a regf log (%02X %02X %02X %02X), left untouched",
                              logFile, magic[0], magic[1], magic[2], magic[3]));
        }
    }

    // ---- 10. 让下次启动检查系统卷 ---------------------------------------------
    //
    // 这就是 chkdsk /f 对在用卷做的事：在 $Volume 里置 VOLUME_IS_DIRTY。默认的 BootExecute
    // （"autocheck autochk *"）只检查带这个标记的卷，所以置上它，下次启动 autochk 就会检查
    // 并修复这个卷 —— 我们绕开文件系统直接改过盘，需要这一道兜底。
    //
    // 为什么不用 FSCTL_MARK_VOLUME_DIRTY：那条路走的是挂载中的文件系统，卷被冻结时写会落进
    // 还原软件的增量区、到不了基线。这里走 handle: → WinDisk 驱动直达磁盘，与其它裸写同一条路。
    //
    // ntfsfix 本来也会置这个标记，但在"文件系统填满整个分区"的卷上它会先 exit(1)
    // （见上面第 8 步的说明），所以只能由我们自己做。
    // 不需要改 BootExecute：置上标记就够了，chkdsk 跑完会自己清掉它，不会每次启动都检查。
    {
        int exitCode = -1;
        std::wstring output;
        std::wstring dirtyError;
        if (SetNtfsVolumeDirty(device.raw(), vol, exitCode, output, dirtyError)) {
            emit.step(L"ntfs-3g-cli setdirty output:\n" + output);
            if (exitCode != 0) {
                emit.warn(FormatW(L"ntfs-3g-cli setdirty exited with %d; the volume may not be "
                                  L"checked at the next boot", exitCode));
            } else {
                emit.ok(L"the system volume is marked dirty -- autochk will check it at the next boot");
            }
        } else {
            emit.warn(Fail(L"mark the system volume dirty", dirtyError));
        }
    }

    // ---- 10. 复位 --------------------------------------------------------------
    //
    // 自动复位会把机器立刻打下去 —— 操作者来不及看日志、也来不及用别的工具核对现场。
    // manualBugcheck 时先等人按一次回车（CLI 模式默认如此），看清了再触发。
    // 到这里说明主 hive、RegBack（若存在）都已逐字节验证；只有此时才允许置 dirty 和复位。

    // 所有 raw I/O 已完成；现在才关闭设备并卸载 WinDisk。这样活动 SYSTEM 不会留下
    // Services\\WinDisk，也不会在下次启动触发“Windows 要求已数字签名的驱动程序”提示。
    device.Close();
    if (UnloadDriver(kServiceName)) {
        emit.ok(L"unloaded WinDisk and removed its transient service entry");
    } else {
        emit.warn(L"could not unload/remove WinDisk service entry; do not reboot if the live "
                  L"registry still contains Services\\WinDisk");
        return result;
    }
    if (opt.manualBugcheck) {
        emit.warn(L"all writes are done. The machine has NOT been reset yet -- press Enter to "
                  L"trigger the bugcheck (0x0D000721 + CTL_REBOOT_SYSTEM)");

        // 复位的作用说清楚（这里以前写着"内存注册表会刷回覆盖我们写的 hive" —— **那是错的**）：
        // 在冻结的机器上，Windows 自己的任何写入都走磁盘栈、被冰点重定向到增量区，到不了基线，
        // 所以它**盖不掉**我们写上去的那份 hive。本轮的目的（让过滤驱动下次启动不再加载）
        // 只取决于基线被写成了什么，**按一次普通重启同样能生效**。
        // 想看验证结论再决定：普通重启是安全的 —— 它不会丢掉内存里的注册表。
        emit.warn(L"a normal reboot achieves the same thing here: on a frozen volume Windows' own "
                  L"writes are redirected, so they cannot overwrite what we put on the baseline. "
                  L"If the read-back did not verify, prefer the normal reboot -- it cannot lose the "
                  L"in-memory registry, and whether the filter drivers stop loading afterwards is "
                  L"the real answer to whether the write landed");

        // 读一行；stdin 不可用（被重定向/没有控制台）时**不**触发，把决定权交回给人。
        std::string line;
        if (std::getline(std::cin, line)) {
            emit.warn(L"resetting now (manual trigger)");
            error.clear();
            Trip(emit, device, error);
            emit.warn(FormatW(L"bugcheck did not reset the machine: %ls", error.c_str()));
        } else {
            emit.warn(L"stdin is not readable (no console / redirected) -- the bugcheck was NOT "
                      L"triggered; reboot whenever you are ready");
        }
        return result;
    }

    emit.warn(L"all writes complete and verified; triggering the reboot bugcheck now");
    error.clear();
    const bool tripped = Trip(emit, device, error);
    // RebootNow 成功即不返回（bugcheck 会停住内核）
    emit.warn(FormatW(L"bugcheck did not reset the machine: %ls", error.c_str()));
    (void)tripped;
    return result;
}

MeltResult DryRun(const MeltOptions& opt, const MeltLogger& log) {
    MeltOptions dry = opt;
    dry.dryRun = true;
    dry.confirm = nullptr;
    return RunMelt(dry, log);
}

// --preflight：一条只用一次的自动判读链 —— **不写 SYSTEM、不碰 RegBack、不复位**，
// 只为用户那句"位置扫描实验"产出完整的、证明自己写的数据会否停留在目标路径的证据。
//
// 顺序有意如此：先拿下 raw 设备通路（与 melt 同一步），**不**动注册表；然后
//   A) logstate 守卫（v2.0 = "Windows 持有缓存元数据"，之前 Win10 就在这一
//      步被挡）
//   B) 目录位置扫描：同一份可验证字节分别写到卷根、\Windows、
//      \Windows\System32、\Windows\System32\config —— 每个位置完整
//      write → read-back → read-head-compare → info: + record: 取证。
//      深度分界线就是答案：只有 config\ 挂 = 硬碟上某个过滤在踩 config；
//      \Windows\System32 挂 = 通用 MFT 记录写问题（回到驱动）；
//      \Windows 就干净 = 与还原栈在 config\ 的实时拦截有关；
//   C) 任一位置失败，自动触发现成的 DiagnoseWritePath 三级拆解
//      （4 MiB new file / 同内容全尺寸新名 / 大小二分），把证据全留在日志里。
// 最后在任何写动作完成前卸载并删除 WinDisk 服务，机器回到它来的样子。
MeltResult RunPreflightScan(const MeltOptions& opt, const MeltLogger& log) {
    // 这类选项在本入口里和 reset / confirm 没任何关系：dry-run 日志
    // 不允许被手动按 Enter（这里也不响是不是有人给了那种选项）。
    (void)opt.manualBugcheck;
    (void)opt.confirm;
    MeltResult result;
    const Log emit{&result, log};
    const std::filesystem::path exeDir = opt.exeDir.empty() ? ExeDir() : opt.exeDir;

    emit.warn(L"--preflight: AUTO root-cause scan (no SYSTEM/RegBack write, no reset)");
    emit.warn(L"this does raw disk I/O under the same handle: protocol as a real melt, "
              L"and it loads an unsigned driver. Only run it on a VM with a snapshot.");

    // ---- 1/2. 装载驱动，判 DSE（与 RunMelt 共用一份实现）
    const DriverLoadOutcome load = EnsureUnsignedDriverLoads(exeDir, /*allowKdu=*/true);
    if (!load.firstError.empty()) emit.warn(Fail(L"load driver", load.firstError));
    if (load.kduRan) {
        emit.warn(L"DSE is still enforced; disabling it via KDU and retrying the load");
        size_t begin = 0;
        while (begin < load.kduLog.size()) {
            const size_t nl = load.kduLog.find(L'\n', begin);
            const std::wstring one(load.kduLog, begin,
                                   nl == std::wstring::npos ? std::wstring::npos : nl - begin);
            if (!one.empty()) emit.step(one);
            if (nl == std::wstring::npos) break;
            begin = nl + 1;
        }
    }
    if (load.result != DriverLoad::Loaded) {
        emit.warn(Fail(L"preflight requires the driver", L"could not load WinDisk"));
        return result;
    }
    emit.ok(load.kduRan ? L"driver loaded after kdu -dse 0" : L"driver loaded without kdu");

    // ---- 3. 打开设备、定位卷（RunMelt 第 2/3 步的同一顺序）
    WinDiskDevice device;
    VolumeInfo vol;
    std::wstring error;
    if (!device.Open(error)) {
        emit.warn(Fail(L"open device", error));
        return result;
    }
    emit.ok(FormatW(L"opened device %ls", DeviceSymbolicLink()));
    if (!QueryVolumeInfo(kSystemVolume, vol, error)) {
        emit.warn(Fail(L"query volume info", error));
        device.Close();
        return result;
    }
    emit.ok(FormatW(L"volume %ls: disk=%u offset=%llu length=%llu (bps=%u spc=%u)",
                    kSystemVolume, vol.diskNumber, vol.volumeOffset, vol.volumeLength,
                    vol.bytesPerSector, vol.sectorsPerCluster));
    if (!device.SetTargetDisk(vol.diskNumber, error)) {
        emit.warn(Fail(L"CTL_CHANGE_TARGET_DISK", error));
        device.Close();
        return result;
    }
    emit.ok(FormatW(L"target disk set to %u", vol.diskNumber));

    // ---- A. logstate 守卫
    if (!LogfileStateGuard(device, vol, emit)) {
        emit.warn(L"preflight aborted: $LogFile restart page is v2.0 (cached metadata); "
                  "do a real shutdown and retry");
        device.Close();
        if (!UnloadDriver(kServiceName))
            emit.warn(L"could not unload/remove WinDisk after the guard stop");
        return result;
    }

    // ---- B. 目录位置扫描
    // VerifyHiveWrite 的 scratchFile 参数是**文件**路径：读回的 head 都落到它上面
    // （每档探针互城覆盖，互城余会只在故障之后留下本档的 scratch 快照，正好）。
    // DiagnoseWritePath 是另一个函数，它的 scratchDir 才是目录。
    const std::filesystem::path scratchDir = DefaultHivePath().parent_path();
    const std::filesystem::path scratchBack = DefaultHivePath().parent_path() /
                                              L"secmelt-probe-back.bin";
    int probeOk = 0, probeFail = 0;
    for (const wchar_t* ntfsProbe : {
             L"secmelt-probe.hive",                           // 卷根
             L"Windows\\secmelt-probe.hive",                  // \Windows（环境目录，无配置）
             L"Windows\\System32\\secmelt-probe.hive",        // \Windows\System32
             L"Windows\\System32\\config\\secmelt-probe.hive" // \Windows\System32\config
         }) {
        emit.step(L"----------------------------------------");
        emit.step(FormatW(L"probe target: \\%ls", ntfsProbe));
        {
            std::wstring rmOut;
            if (!NtfsDeleteFile(device.raw(), vol, std::wstring(ntfsProbe), rmOut)) {
                emit.step(L"  rm:" + rmOut);
            }
        }
        // 用 rollback 的那份导出的 prefix 做内容：让它带真正的 hive pattern
        std::filesystem::path srcPath = DefaultHivePath();
        {
            std::ifstream in(srcPath, std::ios::binary);
            if (!in) {
                // 现给自己造一份复验字节：魔性组间字节，拥有 0 与过多缺口
                std::ofstream out(srcPath, std::ios::binary | std::ios::trunc);
                std::vector<unsigned char> blk(4096);
                for (size_t i = 0; i < blk.size(); ++i)
                    blk[i] = static_cast<unsigned char>((i * 31 + 17) & 0xFFu);
                for (int i = 0; i < 256; ++i) out.write((const char*)blk.data(), blk.size());
            }
        }
        int exitCode = -1;
        std::wstring output, writeError;
        const bool wrote = NtfsCopyIn(device.raw(), vol, srcPath, std::wstring(ntfsProbe), exitCode,
                                      output, writeError) && exitCode == 0;
        if (!wrote) {
            emit.warn(Fail(FormatW(L"write \\%ls", ntfsProbe),
                           FormatW(L"exit=%d: %ls", exitCode,
                                   writeError.empty() ? output.c_str() : writeError.c_str())));
            ++probeFail;
            continue;
        }
        emit.ok(L"ntfscp exit=0; starting read-back");
        const bool ok = VerifyHiveWrite(device, vol, std::wstring(ntfsProbe), srcPath,
                                        scratchBack, emit);
        // 总是加餐取证：即便碰巧通过，也给后面扫根的取证留全泰
        {
            std::wstring info;
            if (NtfsAttrInfo(device.raw(), vol, std::wstring(ntfsProbe), info)) {
                emit.step(L"  info: " + info);
            }
            std::wstring rec;
            if (NtfsRecordDump(device.raw(), vol, std::wstring(ntfsProbe), rec)) {
                emit.step(L"  record: " + rec);
            }
        }
        {
            std::wstring rmOut;
            if (!NtfsDeleteFile(device.raw(), vol, std::wstring(ntfsProbe), rmOut)) {
                emit.warn(Fail(L"probe cleanup", rmOut));
            }
        }
        if (ok) {
            emit.ok(FormatW(L"PASS at \\%ls", ntfsProbe));
            ++probeOk;
        } else {
            emit.warn(FormatW(L"FAIL at \\%ls -- driving into DiagnoseWritePath for the "
                              L"three-layer breakdown", ntfsProbe));
            // Group 2 全尺寸（同一份字节到新名，不带写 /Windows/ 1MiB 线）
            DiagnoseWritePath(device, vol, srcPath, scratchDir, emit);
            ++probeFail;
        }
    }

    // ---- 收尾：卸载驱动
    device.Close();
    if (UnloadDriver(kServiceName)) {
        emit.ok(L"unloaded WinDisk and removed its transient service entry");
    } else {
        emit.warn(L"could not unload/remove WinDisk service entry");
    }

    // 与 melt 的 ok 语义对齐：preflight 没写 SYSTEM/RegBack/复位，所以 wroteDisk 恒 false；
    // ok=true 只在"所有探测位置都干净往返"时。调用方据此决定还敢不敢继续 melt。
    result.wroteDisk = false;
    result.ok = (probeFail == 0);
    emit.warn(FormatW(L"scan complete: %d pass, %d fail at probe depths (%ls)",
                      probeOk, probeFail,
                      result.ok ? L"preflight green" : L"root-cause located at the marked depth"));
    return result;
}

}  // namespace secmelt
