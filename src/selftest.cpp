#include "selftest.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "melt/unfreeze.h"
#include "raw/ntfs_tool.h"
#include "raw/win_disk.h"
#include "reg/hive.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

namespace secmelt {
namespace {

void Print(const std::wstring& line) { std::printf("%s\n", Narrow(line).c_str()); }

void Print(const char* line) { std::printf("%s\n", line); }

std::filesystem::path TempFile(const wchar_t* name) {
    std::vector<wchar_t> temp(MAX_PATH);
    const DWORD need = ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    const std::filesystem::path dir = (need > 0 && need < temp.size())
                                          ? std::filesystem::path(temp.data())
                                          : std::filesystem::path(L"C:\\Windows\\Temp");
    return dir / name;
}

}  // namespace

int HiveSelfTest(const std::filesystem::path& exeDir) {
    (void)exeDir;
    const std::filesystem::path hive = TempFile(L"secmelt-selftest.hive");
    Print(L"SecMelt hive self-test");
    Print(FormatW(L"target: %ls", hive.c_str()));

    std::wstring error;
    if (!ExportSystemHive(hive, error)) {
        Print(FormatW(L"FAIL export: %ls", error.c_str()));
        return 1;
    }
    Print(L"exported SYSTEM hive (RegSaveKeyEx, REG_LATEST_FORMAT)");

    HiveBaseBlock before;
    if (!ReadBaseBlock(hive, before, error)) {
        Print(FormatW(L"FAIL read base block: %ls", error.c_str()));
        return 1;
    }
    Print(L"--- base block as saved ---");
    Print(FormatW(L"signature        : %ls", before.signatureOk ? L"regf" : L"INVALID"));
    Print(FormatW(L"sequence1        : %u", before.sequence1));
    Print(FormatW(L"sequence2        : %u", before.sequence2));
    Print(FormatW(L"major.minor      : %u.%u", before.major, before.minor));
    Print(FormatW(L"type             : %u (0 = primary hive)", before.type));
    Print(FormatW(L"format           : %u", before.format));
    Print(FormatW(L"root cell offset : %u", before.rootCell));
    Print(FormatW(L"hive bins size   : %u", before.binsSize));
    Print(FormatW(L"clustering factor: %u", before.clusteringFactor));
    Print(FormatW(L"stored checksum  : 0x%08X", before.storedChecksum));
    Print(FormatW(L"computed checksum: 0x%08X", before.computedChecksum));
    Print(FormatW(L"file size        : %llu", before.fileSize));
    Print(FormatW(L"bins == size-4K  : %ls", (before.binsSize + kHiveBlockSize == before.fileSize) ? L"yes" : L"no"));

    HiveBaseBlock after;
    if (!MakeCleanAndFixChecksum(hive, after, error)) {
        Print(FormatW(L"FAIL clean/fix: %ls", error.c_str()));
        return 1;
    }
    Print(FormatW(L"base block rewritten=%ls",
                  (before.sequence2 != after.sequence2 || before.storedChecksum != after.storedChecksum)
                      ? L"yes"
                      : L"no (already clean and valid)"));

    bool ok = true;
    const auto check = [&](const wchar_t* name, bool value) {
        Print(FormatW(L"%-34ls: %ls", name, value ? L"OK" : L"FAILED"));
        if (!value) ok = false;
    };
    Print(L"--- assertions ---");
    check(L"signature is \"regf\"", after.signatureOk);
    check(L"computedChecksum == storedChecksum", after.checksumOk);
    check(L"sequence1 == sequence2", after.clean);
    check(L"fileSize % 4096 == 0", after.fileSize % kHiveBlockSize == 0);
    check(L"root cell is an \"nk\" cell", [&] {
        std::wstring verifyError;
        // VerifyHiveLoadable 覆盖同一批断言，这里直接复用它来判定 root cell
        return VerifyHiveLoadable(hive, verifyError);
    }());
    if (!ok) Print(L"FAILED: regf layout constants or checksum algorithm are wrong");
    Print(ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}

int RegistrySelfTest(const std::filesystem::path& exeDir) {
    // scratch 键落在 HKLM\SOFTWARE 下：结构与设备类键相同（同一段代码处理），
    // 但绝不涉及任何设备类，因此不可能影响下次启动的设备/驱动加载。
    constexpr const wchar_t* kScratchRoot = L"SOFTWARE\\SecMelt\\Selftest";
    constexpr const wchar_t* kFakeGuid = L"{00000000-0000-0000-0000-0000000000FF}";
    const std::wstring classPath = std::wstring(kScratchRoot) + L"\\Class\\" + kFakeGuid;

    Print(L"SecMelt registry self-test");
    Print(FormatW(L"scratch key: HKLM\\%ls (no device class key is touched)", classPath.c_str()));

    ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kScratchRoot);  // 从干净状态开始

    HKEY key = nullptr;
    LSTATUS status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, classPath.c_str(), 0, nullptr, 0,
                                       KEY_ALL_ACCESS, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        Print(FormatW(L"FAIL create scratch key: %ld (registry self-test needs elevation)", status));
        return 1;
    }

    const auto setValue = [&](const wchar_t* name, const std::vector<std::wstring>& items) {
        std::vector<wchar_t> buffer;
        for (const auto& item : items) {
            buffer.insert(buffer.end(), item.begin(), item.end());
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');
        return ::RegSetValueExW(key, name, 0, REG_MULTI_SZ,
                                reinterpret_cast<const BYTE*>(buffer.data()),
                                static_cast<DWORD>(buffer.size() * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };
    const auto getValue = [&](const wchar_t* name, std::vector<std::wstring>& items) {
        items.clear();
        DWORD type = 0;
        DWORD bytes = 0;
        if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
            return false;
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
        if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()),
                               &bytes) != ERROR_SUCCESS)
            return false;
        const wchar_t* p = buffer.data();
        const wchar_t* end = buffer.data() + bytes / sizeof(wchar_t);
        while (p < end && *p != L'\0') {
            items.emplace_back(p);
            p += items.back().size() + 1;
        }
        return true;
    };

    // 名单 = 真实名单 + SecMeltProbe；插入的项里故意含一个大小写变体（DEEPFRZ）来验证不区分大小写
    auto names = TargetNames(LoadTargets(exeDir));
    names.emplace_back(L"SecMeltProbe");

    bool ok = true;
    const auto check = [&](const wchar_t* what, bool value) {
        Print(FormatW(L"%-46ls: %ls", what, value ? L"OK" : L"FAILED"));
        if (!value) ok = false;
    };

    // 用例 1：列表里混有系统自身的项 + 命中项 → 只删命中项，其余原样保留且顺序不变
    setValue(L"UpperFilters", {L"partmgr", L"SecMeltProbe", L"DEEPFRZ", L"OtherFilter"});
    EditReport report = StripFilterValueIn(classPath, kFakeGuid, names);
    std::vector<std::wstring> items;
    const bool present = getValue(L"UpperFilters", items);
    Print(FormatW(L"UpperFilters now: %zu item(s)", items.size()));
    for (const auto& item : items) Print(L"  " + item);
    check(L"matched entries removed, others kept", present && items.size() == 2 &&
                                                       items[0] == L"partmgr" &&
                                                       items[1] == L"OtherFilter");
    check(L"report lists the changed value", report.changedValues.size() == 1);
    check(L"no write failures reported", report.failures.empty());

    // 用例 2：列表里全是命中项 → 整值删除（而不是留下空数组）
    setValue(L"LowerFilters", {L"SecMeltProbe", L"deEPfrZ"});
    const EditReport emptyCase = StripFilterValueIn(classPath, kFakeGuid, names);
    std::vector<std::wstring> lower;
    check(L"all-matched value is deleted entirely", !getValue(L"LowerFilters", lower));
    check(L"deletion is reported as a change", emptyCase.changedValues.size() == 1);

    // 用例 2b：值是单项 REG_SZ（部分安装程序这么写）→ 必须能读出来、能删掉、类型不被改写成 MULTI_SZ
    {
        const wchar_t* probe = L"SecMeltProbe";
        const DWORD bytes = static_cast<DWORD>((wcslen(probe) + 1) * sizeof(wchar_t));
        const bool wrote = ::RegSetValueExW(key, L"LowerFilters", 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(probe), bytes) ==
                           ERROR_SUCCESS;
        const EditReport szCase = StripFilterValueIn(classPath, kFakeGuid, names);
        DWORD type = 0;
        DWORD size = 0;
        const bool gone = ::RegQueryValueExW(key, L"LowerFilters", nullptr, &type, nullptr, &size) ==
                          ERROR_FILE_NOT_FOUND;
        check(L"REG_SZ single item is matched and removed (not skipped)",
              wrote && szCase.changedValues.size() == 1 && szCase.failures.empty() && gone);
    }

    // 用例 2c：REG_SZ 列表里含未命中项 → 写回时必须仍是 REG_SZ（不能悄悄升级成 MULTI_SZ）
    {
        const wchar_t* value = L"partmgr";
        const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
        ::RegSetValueExW(key, L"LowerFilters", 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
        const EditReport szKept = StripFilterValueIn(classPath, kFakeGuid, names);
        check(L"REG_SZ without a match stays untouched", szKept.changedValues.empty());
    }

    // 用例 3：列表里没有命中项 → 一个字节都不动
    setValue(L"UpperFilters", {L"partmgr", L"OtherFilter"});
    const EditReport untouched = StripFilterValueIn(classPath, kFakeGuid, {L"SomethingElse"});
    check(L"no match means no change reported", untouched.changedValues.empty());

    ::RegCloseKey(key);
    ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kScratchRoot);
    HKEY leftover = nullptr;
    const bool removed =
        ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kScratchRoot, 0, KEY_READ, &leftover) != ERROR_SUCCESS;
    if (!removed) ::RegCloseKey(leftover);
    check(L"scratch key cleaned up", removed);

    Print(ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}

int RawSelfTest(const std::filesystem::path& exeDir) {
    Print(L"SecMelt raw disk self-test");
    Print(L"WARNING: this loads a kernel driver and writes to the raw disk of the system volume.");

    const std::filesystem::path scratch = L"C:\\secmelt-selftest.bin";
    constexpr size_t kScratchSize = 64 * 1024;
    constexpr unsigned char kPatternA = 0xA5;
    constexpr unsigned char kPatternB = 0x5A;

    std::wstring error;
    const std::filesystem::path sysPath = exeDir / L"WinDisk_x64.sys";
    if (!LoadDriver(sysPath, L"WinDisk", error)) {
        Print(FormatW(L"FAIL load driver: %ls", error.c_str()));
        return 1;
    }
    Print(L"driver loaded");

    WinDiskDevice device;
    if (!device.Open(error)) {
        Print(FormatW(L"FAIL open device: %ls", error.c_str()));
        return 1;
    }

    VolumeInfo vol;
    if (!QueryVolumeInfo(L"\\\\.\\C:", vol, error)) {
        Print(FormatW(L"FAIL volume info: %ls", error.c_str()));
        return 1;
    }
    Print(FormatW(L"volume: disk=%u offset=%llu length=%llu bps=%u spc=%u", vol.diskNumber,
                  vol.volumeOffset, vol.volumeLength, vol.bytesPerSector, vol.sectorsPerCluster));

    if (!device.SetTargetDisk(vol.diskNumber, error)) {
        Print(FormatW(L"FAIL CTL_CHANGE_TARGET_DISK: %ls", error.c_str()));
        return 1;
    }

    // 用普通 API 造一个已知内容的文件，再通过驱动按物理区段改写、用普通 API 读回
    bool ok = true;
    {
        HANDLE file = ::CreateFileW(scratch.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            Print(FormatW(L"FAIL create %ls: %u", scratch.c_str(), ::GetLastError()));
            return 1;
        }
        std::vector<unsigned char> buffer(kScratchSize, kPatternA);
        DWORD written = 0;
        ok = ::WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) &&
             written == buffer.size();
        // 把 A 真正刷到盘上再关句柄：这样文件系统缓存是"干净的"，后面裸写进去的 B
        // 不会再被缓存里的脏页覆盖，读回结果才有意义。
        if (ok) ok = ::FlushFileBuffers(file) != FALSE;
        ::CloseHandle(file);
        if (!ok) {
            Print(L"FAIL write pattern A through the normal API");
            return 1;
        }
    }
    Print(L"scratch file created with pattern 0xA5 (flushed to disk)");

    std::vector<Extent> extents;
    if (!ListExtentsPhysical(scratch, vol, extents, error)) {
        Print(FormatW(L"FAIL enumerate extents: %ls", error.c_str()));
        ::DeleteFileW(scratch.c_str());
        return 1;
    }
    Print(FormatW(L"physical extents: %zu", extents.size()));
    for (size_t i = 0; i < extents.size(); ++i) {
        Print(FormatW(L"  #%zu offset=%llu length=%llu", i + 1, extents[i].physicalOffset,
                      extents[i].length));
    }
    ok = !extents.empty();
    if (!ok) Print(L"FAIL no extents");

    std::vector<unsigned char> patternB(kScratchSize, kPatternB);
    size_t offset = 0;
    for (const auto& extent : extents) {
        if (!ok) break;
        if (offset >= patternB.size()) break;
        const size_t chunk = static_cast<size_t>(
            (extent.length < patternB.size() - offset) ? extent.length : patternB.size() - offset);
        if (!device.WriteAt(extent.physicalOffset, patternB.data() + offset, chunk, error)) {
            Print(FormatW(L"FAIL raw write: %ls", error.c_str()));
            ok = false;
            break;
        }
        offset += chunk;
    }
    if (ok && offset < patternB.size()) {
        Print(FormatW(L"FAIL extents cover only %zu of %zu bytes", offset, patternB.size()));
        ok = false;
    }

    // 读回校验必须绕开文件系统缓存 —— 这是第一次跑这条自检时踩的坑：
    // 裸写是直达设备（不经过 FS 缓存），而普通 ReadFile 会命中缓存里的旧页，
    // 于是"写成功、读回还是旧图案"，看起来像驱动没写，其实写对了。
    if (ok) {
        // (1) 用同一条裸盘通路读回：证明写确实落在这些物理偏移上。
        std::vector<unsigned char> rawRead(kScratchSize, 0);
        size_t read = 0;
        for (const auto& extent : extents) {
            if (read >= rawRead.size()) break;
            const size_t chunk =
                static_cast<size_t>((extent.length < rawRead.size() - read) ? extent.length
                                                                           : rawRead.size() - read);
            if (chunk == 0) continue;
            if (!device.ReadAt(extent.physicalOffset, rawRead.data() + read, chunk, error)) {
                Print(FormatW(L"FAIL raw read-back: %ls", error.c_str()));
                ok = false;
                break;
            }
            read += chunk;
        }
        if (ok) {
            for (size_t i = 0; i < rawRead.size(); ++i) {
                if (rawRead[i] != kPatternB) {
                    Print(FormatW(L"FAIL raw read-back mismatch at byte %zu: 0x%02X", i, rawRead[i]));
                    ok = false;
                    break;
                }
            }
            if (ok) Print(L"raw read-back matches pattern 0x5A");
        }
    }

    if (ok) {
        // (2) 再从文件系统侧读一次，但用 FILE_FLAG_NO_BUFFERING 绕过缓存：
        // 证明这些物理区段确实就是这个文件的数据（FSCTL 的映射没算错）。
        // 无缓冲 I/O 要求缓冲区按扇区对齐（VirtualAlloc 给的是 64 KiB 对齐）且
        // 长度是扇区整数倍（64 KiB ✓）。
        const DWORD sector = vol.bytesPerSector ? vol.bytesPerSector : 512;
        void* aligned = ::VirtualAlloc(nullptr, kScratchSize, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
        HANDLE file = ::CreateFileW(scratch.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_NO_BUFFERING, nullptr);
        if (!aligned || file == INVALID_HANDLE_VALUE) {
            Print(FormatW(L"FAIL open scratch uncached: %u", ::GetLastError()));
            ok = false;
        } else {
            DWORD got = 0;
            ok = ::ReadFile(file, aligned, static_cast<DWORD>(kScratchSize), &got, nullptr) &&
                 got == kScratchSize;
            ::CloseHandle(file);
            if (ok) {
                const auto* bytes = static_cast<const unsigned char*>(aligned);
                for (size_t i = 0; i < kScratchSize; ++i) {
                    if (bytes[i] != kPatternB) {
                        Print(FormatW(L"FAIL uncached FS read-back mismatch at byte %zu: 0x%02X", i,
                                      bytes[i]));
                        ok = false;
                        break;
                    }
                }
                if (ok) Print(L"uncached file-system read-back matches pattern 0x5A");
            } else {
                Print(FormatW(L"FAIL uncached read (sector=%u): %u", sector, ::GetLastError()));
            }
        }
        if (aligned) ::VirtualFree(aligned, 0, MEM_RELEASE);
    }

    ::DeleteFileW(scratch.c_str());
    Print(ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}

int MeltApply(const std::filesystem::path& exeDir, bool runNtfsFix) {
    // 非交互落盘。确认契约在这里的体现：调用方必须先给出 --yes-i-know，
    // 否则 main() 根本不会走到这里；因此这个回调恒为 true 是"用户已确认"的直接后果，
    // 而不是把确认步骤删掉（TUI 那条路仍然弹框）。
    MeltOptions opt;
    opt.dryRun = false;
    opt.exeDir = exeDir;
    opt.runNtfsFix = runNtfsFix;
    opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";
    opt.confirm = [](const MeltResult&) { return true; };

    Print(L"SecMelt melt (non-interactive; --yes-i-know was given)");
    Print(L"WARNING: this writes the SYSTEM hive to the raw disk, zeroes the transaction");
    Print(L"         logs and then resets the machine by bugcheck. There is no rollback.");
    const MeltResult result = RunMelt(opt, [](const std::wstring& line) { Print(line); });
    if (!result.pending.empty()) {
        Print(L"--- planned actions ---");
        for (const auto& item : result.pending) Print(L"  " + item);
    }
    if (!result.ok) {
        // ok 仅在 dryRun 时为 true；落盘路径走到 bugcheck 就再也不会返回，
        // 所以"走到这里且 ok=false"意味着中途失败（日志里已有原因）。
        Print(result.wroteDisk
                  ? L"FAILED: the raw disk was already written - state may be inconsistent, "
                    L"reboot now or restore from backup"
                  : L"FAILED: nothing was written to the raw disk");
        return 1;
    }
    Print(L"PASS");
    return 0;
}

int MeltDryRun(const std::filesystem::path& exeDir, bool runNtfsFix) {
    MeltOptions opt;
    opt.dryRun = true;
    opt.exeDir = exeDir;
    opt.runNtfsFix = runNtfsFix;
    opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";

    MeltResult result = DryRun(opt, nullptr);
    for (const auto& line : result.log) Print(line);
    Print(L"--- planned actions (no raw disk write happened) ---");
    for (const auto& item : result.pending) Print(L"  " + item);
    Print(result.ok ? L"PASS" : L"FAIL");
    return result.ok ? 0 : 1;
}

}  // namespace secmelt
