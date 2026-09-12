#include "melt/unfreeze.h"

#include <windows.h>

#include <cstring>
#include <system_error>

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
constexpr const wchar_t* kLogFiles[] = {
    L"C:\\Windows\\System32\\config\\SYSTEM.LOG1",
    L"C:\\Windows\\System32\\config\\SYSTEM.LOG2",
};
// 日志头部被清零的长度：regf 事务日志的 base block 就位于文件偏移 0，
// 清掉它即让日志失去有效头，任何日志项都无法再被应用。
constexpr uint64_t kLogHeaderBytes = 4096;
// 与 third_party/WinDisk/Main.cpp 的 SECMELT_BUGCHECK_CODE 必须一致：
// CTL_REBOOT_SYSTEM 触发这个 bugcheck，而不是走常规关机路径。
constexpr unsigned long kBugCheckCode = 0x0D000721;

struct Log {
    MeltResult* result;
    MeltLogger sink;
    void operator()(const std::wstring& line) const {
        result->log.push_back(line);
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

// 通过 KDU 关闭驱动签名强制（DSE），供 apply 路径自动调用。
//
// 为什么需要它：WinDisk.sys 未签名，DSE 开着就装不上，整条链路走不下去。原先是把
// `kdu -dse 0` 当作外部前置要求人手工执行，现在一键 melt 自己完成。
//
// 两个实现要点：
//   * 判据不是 kdu 的退出码 —— 它的 ControlDSE 回调返回的是不同 provider 各自的 BOOL，
//     失败路径也可能返回 0，退出码不可靠。这里跑完 kdu 后**自己重新探测 DSE**，
//     以真实状态为准。
//   * kdu 需要与它同目录的 drv64.dll（provider 数据库）。缺了它 kdu 会退回 Hamakaze 里
//     那份空的内嵌表，输出 `Provider: "(null)"` + `Driver resource id cannot be found`，
//     然后静默什么都不做 —— 所以这里先显式检查 drv64.dll，把这种情况变成一条明确的错误。
bool DisableDseWithKdu(const std::filesystem::path& exeDir, const Log& log, std::wstring& error) {
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
    log.step(FormatW(L"kdu -dse 0 exited with %d", exitCode));
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

    bool disabled = false;
    if (!DseDisabled(disabled, error)) return false;
    if (!disabled) {
        error = FormatW(L"kdu reported exit code %d but driver signature enforcement is still ON "
                        L"(HVCI / memory integrity enabled, or the selected provider does not "
                        L"work on this build)",
                        exitCode);
        return false;
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

// 清零文件头部 kLogHeaderBytes 字节，并读回断言全零。
bool ClearFileHeader(WinDiskDevice& device, const std::filesystem::path& file, const VolumeInfo& vol,
                     const Log& log, std::wstring& error) {
    std::vector<Extent> extents;
    if (!ListExtentsPhysical(file, vol, extents, error)) return false;

    // 按段依次写零：首段可能不足 4096 字节，跨段继续（与原作者 PatchFile 的分段循环同形）
    std::vector<unsigned char> zeros(static_cast<size_t>(kLogHeaderBytes), 0);
    size_t written = 0;
    for (const auto& extent : extents) {
        if (written >= zeros.size()) break;
        const size_t chunk =
            static_cast<size_t>((extent.length < zeros.size() - written) ? extent.length
                                                                        : zeros.size() - written);
        if (chunk == 0) continue;
        if (!device.WriteAt(extent.physicalOffset, zeros.data() + written, chunk, error)) {
            return false;
        }
        log.step(FormatW(L"zeroed %zu bytes of %ls at physical offset %llu", chunk,
                         file.filename().c_str(), extent.physicalOffset));
        written += chunk;
    }
    if (written < zeros.size()) {
        error = FormatW(L"%ls provides only %zu bytes of extents; need %zu", file.filename().c_str(),
                        written, zeros.size());
        return false;
    }

    std::vector<unsigned char> verify(zeros.size(), 0xFF);
    size_t read = 0;
    for (const auto& extent : extents) {
        if (read >= verify.size()) break;
        const size_t chunk =
            static_cast<size_t>((extent.length < verify.size() - read) ? extent.length
                                                                      : verify.size() - read);
        if (chunk == 0) continue;
        if (!device.ReadAt(extent.physicalOffset, verify.data() + read, chunk, error)) return false;
        read += chunk;
    }
    for (size_t i = 0; i < verify.size(); ++i) {
        if (verify[i] != 0) {
            error = FormatW(L"read-back of %ls is not zero at byte %zu (0x%02X)",
                            file.filename().c_str(), i, verify[i]);
            return false;
        }
    }
    return true;
}

}  // namespace

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

    bool dseOff = false;
    if (!DseDisabled(dseOff, error)) {
        emit.warn(Fail(L"preflight / DSE query", error));
        return result;
    }
    // DSE（驱动签名强制）开着时未签名的 WinDisk.sys 装不上，所以 apply 路径先自己用
    // KDU 关掉它（kdu.exe + drv64.dll 就在 exe 旁边，由构建期落位）。
    const bool dseWasOn = !dseOff;
    if (dseWasOn) {
        if (opt.dryRun) {
            // dry-run 只读预演：不改系统状态，只把这一步列进"将会执行的动作"。
            emit.warn(L"DSE is ENABLED; dry-run will not change it (apply runs 'kdu -dse 0')");
        } else {
            emit.warn(L"DSE is ENABLED; disabling it via KDU before touching the disk");
            if (!DisableDseWithKdu(exeDir, emit, error)) {
                emit.warn(Fail(L"disable DSE", error));
                return result;
            }
            emit.ok(L"DSE state: disabled (by kdu -dse 0)");
        }
    } else {
        emit.ok(L"DSE state: disabled");
    }

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
    if (!opt.dryRun) {
        if (!LoadDriver(sysPath, kServiceName, error)) {
            emit.warn(Fail(L"load driver", error));
            return result;
        }
        emit.ok(FormatW(L"driver service '%ls' started", kServiceName));

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
            emit.warn(FormatW(L"would delete service key: %ls", ServiceKeyPath(key).c_str()));
        }
        for (const auto& key : probe.absentServiceKeys) {
            emit.step(FormatW(L"service key absent: %ls", key.c_str()));
        }
    } else {
        const EditReport filters = StripFilterEntries(names);
        for (const auto& v : filters.changedValues) emit.ok(FormatW(L"rewrote filter value: %ls", v.c_str()));
        if (filters.changedValues.empty()) emit.step(L"no filter entry matched the target list");

        const EditReport services = DeleteServiceKeys(names);
        for (const auto& k : services.deletedKeys) emit.ok(FormatW(L"deleted service key: %ls", k.c_str()));
        for (const auto& k : services.absentKeys) emit.step(FormatW(L"service key absent: %ls", k.c_str()));
        if (services.deletedKeys.empty()) emit.step(L"no target service key existed in the registry");

        for (const auto& f : filters.failures) emit.warn(Fail(L"filter value", f));
        for (const auto& f : services.failures) emit.warn(Fail(L"service key", f));
        if (!filters.failures.empty() || !services.failures.empty()) {
            emit.warn(L"registry stripping failed; aborting before any raw disk write");
            return result;
        }
    }

    // ---- 6. 导出 hive，校验/修补 base block -----------------------------------
    const std::filesystem::path hivePath =
        opt.hiveOutPath.empty() ? DefaultHivePath() : opt.hiveOutPath;
    if (!ExportSystemHive(hivePath, error)) {
        emit.warn(Fail(L"export SYSTEM hive", error));
        return result;
    }
    emit.ok(FormatW(L"exported SYSTEM hive to %ls", hivePath.c_str()));

    HiveBaseBlock before;
    if (!ReadBaseBlock(hivePath, before, error)) {
        emit.warn(Fail(L"read base block", error));
        return result;
    }
    emit.step(L"base block (before): " + DescribeBaseBlock(before));

    HiveBaseBlock after;
    if (!MakeCleanAndFixChecksum(hivePath, after, error)) {
        emit.warn(Fail(L"clean / fix checksum", error));
        return result;
    }
    emit.step(L"base block (after): " + DescribeBaseBlock(after));
    if (before.sequence2 != after.sequence2 || before.storedChecksum != after.storedChecksum) {
        emit.warn(L"hive was not clean/valid as saved; base block was rewritten");
    } else {
        emit.ok(L"hive was already clean with a valid checksum; nothing to fix");
    }

    if (!VerifyHiveLoadable(hivePath, error)) {
        emit.warn(Fail(L"verify hive", error));
        return result;
    }
    emit.ok(L"hive verified: signature, type, alignment, sequence numbers, checksum, root cell");

    // ---- 7. 用户确认（此前尚未发生任何裸盘写入）--------------------------------
    if (dseWasOn) {
        // dry-run 时它还没执行；apply 时已经在预检里做完了，列出来让确认清单完整。
        result.pending.push_back(L"disable driver signature enforcement (kdu -dse 0)");
    }
    for (const wchar_t* logFile : kLogFiles) {
        result.pending.push_back(
            FormatW(L"zero the first %llu bytes of %ls", kLogHeaderBytes, logFile));
    }
    result.pending.push_back(
        FormatW(L"copy %ls to %ls on the raw disk", hivePath.c_str(), kNtfsSystemHive));
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
        emit.warn(L"no confirmation callback provided; refusing to write the raw disk");
        return result;
    }
    for (const auto& item : result.pending) emit.warn(L"pending: " + item);
    if (!opt.confirm(result)) {
        emit.warn(L"confirmation declined; nothing was written to the raw disk");
        return result;
    }

    // ---- 8. 写回 hive ---------------------------------------------------------
    if (opt.runNtfsFix) {
        if (ResolveTool(L"ntfsfix.exe").empty()) {
            emit.warn(Fail(L"ntfsfix", L"ntfsfix.exe not found"));
            return result;
        }
        int exitCode = -1;
        std::wstring output;
        if (!NtfsFix(device.raw(), vol, exitCode, output, error)) {
            emit.warn(Fail(L"ntfsfix", error));
            return result;
        }
        emit.step(L"ntfsfix output:\n" + output);
        if (exitCode != 0) {
            emit.warn(FormatW(L"ntfsfix exited with %d; use --no-ntfsfix to skip this step", exitCode));
            return result;
        }
        emit.ok(L"ntfsfix reported success");
    }

    result.wroteDisk = true;  // 从这一行起，失败就意味着磁盘上可能有半份 hive
    // 第 8 步只写不撤销：无论后面哪一步失败，都必须立刻触发 bugcheck —— 让内核带着
    // 内存里那份"干净且校验和有效"的 SYSTEM hive 停住。否则系统继续运行时，注册表懒写回
    // 迟早会把磁盘上刚写好的 hive 覆盖掉，反倒留下一个改了一半的注册表。
    {
        int exitCode = -1;
        std::wstring output;
        if (!NtfsCopyIn(device.raw(), vol, hivePath, kNtfsSystemHive, exitCode, output, error)) {
            emit.warn(Fail(L"ntfscp (write state unknown)", error));
            Trip(emit, device, error);
            return result;
        }
        emit.step(L"ntfscp output:\n" + output);
        if (exitCode != 0 || output.find(L"ERROR") != std::wstring::npos) {
            emit.warn(FormatW(L"ntfscp exited with %d; the hive may be only partially written",
                              exitCode));
            Trip(emit, device, error);
            return result;
        }
        emit.ok(FormatW(L"hive written back to %ls", kNtfsSystemHive));
    }

    // ---- 9. 清零事务日志头部 ---------------------------------------------------
    for (const wchar_t* logFile : kLogFiles) {
        const std::filesystem::path path(logFile);
        if (!secmelt::PathIsRegularFile(path)) {
            emit.step(FormatW(L"%ls does not exist; nothing to clear", logFile));
            continue;
        }
        error.clear();
        if (!ClearFileHeader(device, path, vol, emit, error)) {
            emit.warn(Fail(FormatW(L"clear log header of %ls", logFile), error));
            Trip(emit, device, error);
            return result;
        }
        emit.ok(FormatW(L"%ls header zeroed and verified", logFile));
    }

    // ---- 10. 立即硬重启（此步之前不再有任何提示或等待）------------------------
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

}  // namespace secmelt
