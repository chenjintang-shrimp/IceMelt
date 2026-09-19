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

// 所有 CLI 输出都从这里走：cold & dark 着色（出错醒目红色、[!] 琥珀、其余白色）在同一处
// 决定。重定向/不支持 VT 时 CliPaint 原样返回，日志里不会混入转义序列。
void Print(const std::wstring& line) { std::printf("%s\n", Narrow(CliPaint(line)).c_str()); }

void Print(const char* line) { Print(Widen(line)); }

std::filesystem::path TempFile(const wchar_t* name) {
    std::vector<wchar_t> temp(MAX_PATH);
    const DWORD need = ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    const std::filesystem::path dir = (need > 0 && need < temp.size())
                                          ? std::filesystem::path(temp.data())
                                          : std::filesystem::path(L"C:\\Windows\\Temp");
    return dir / name;
}

// --selftest-raw 用的图案：文件先写成 A，再经裸盘通路改成 B。
constexpr unsigned char kPatternA = 0xA5;
constexpr unsigned char kPatternB = 0x5A;

// 一次读回的图案统计。把"读到了什么"讲成数字，而不是只报第一个不符的字节 ——
// 判断"是旧内容还是别的"靠的就是这两个计数。
struct Readback {
    size_t bytesA = 0;
    size_t bytesB = 0;
    size_t other = 0;
    size_t firstDivergentFromB = 0;
    unsigned char divergentValue = 0;
    bool hasDivergence = false;

    std::wstring Describe() const {
        std::wstring out = FormatW(L"0xA5 x%zu, 0x5A x%zu, other x%zu", bytesA, bytesB, other);
        if (hasDivergence) {
            out += FormatW(L"; first byte that is not 0x5A: #%zu = 0x%02X", firstDivergentFromB,
                           divergentValue);
        }
        return out;
    }
};

Readback Summarize(const unsigned char* bytes, size_t size) {
    Readback out;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] == kPatternA) {
            ++out.bytesA;
        } else if (bytes[i] == kPatternB) {
            ++out.bytesB;
        } else {
            ++out.other;
        }
        if (!out.hasDivergence && bytes[i] != kPatternB) {
            out.hasDivergence = true;
            out.firstDivergentFromB = i;
            out.divergentValue = bytes[i];
        }
    }
    return out;
}

// 按区段拼读一段物理区域（底层通路：驱动直达磁盘）。区段总长不足时返回 false ——
// 那说明 FSCTL 给出的映射覆盖不了这个文件，后面的判据都不成立。
bool ReadExtentsRaw(WinDiskDevice& device, const std::vector<Extent>& extents, size_t want,
                    std::vector<unsigned char>& out, std::wstring& error) {
    out.assign(want, 0);
    size_t read = 0;
    for (const auto& extent : extents) {
        if (read >= want) break;
        const size_t chunk = static_cast<size_t>((extent.length < want - read) ? extent.length
                                                                              : want - read);
        if (chunk == 0) continue;
        if (!device.ReadAt(extent.physicalOffset, out.data() + read, chunk, error)) return false;
        read += chunk;
    }
    if (read < want) {
        error = FormatW(L"the extents cover only %zu of %zu bytes", read, want);
        return false;
    }
    return true;
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

    std::wstring error;
    const std::filesystem::path sysPath = exeDir / L"WinDisk_x64.sys";
    const DriverLoad load = LoadDriver(sysPath, L"WinDisk", error);
    if (load != DriverLoad::Loaded) {
        Print(FormatW(L"FAIL load driver: %ls", error.c_str()));
        if (load == DriverLoad::SignatureRejected) {
            Print(L"     the load was rejected with 577 (ERROR_INVALID_IMAGE_HASH): run "
                  L"'kdu -dse 0' first, then retry -- a positive DSE state read out of "
                  L"NtQuerySystemInformation does not mean the load will be allowed");
        }
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

    // 读回分两条路，判据完全不同 —— 报告里必须讲清每条走的是哪条路：
    //   (1) 底层读回：经 WinDisk 驱动直达磁盘 miniport。它证明的是"写确实落到了这些
    //       物理偏移上"，与文件系统看到的无关。
    //   (2) OS 读回：经文件系统（FILE_FLAG_NO_BUFFERING 绕开缓存，但**不绕过滤器**）。
    //       它证明的是"这台机器上文件系统看到的文件内容"。
    // 冻结状态（冰点/影子系统这类卷过滤器）下两者**必然**不一致：文件系统的写被过滤器
    // 留在上层，驱动却能直接改磁盘，于是底层读到 B、OS 侧仍读到 A。这不是失败，恰恰是
    // 过滤器在工作 —— 所以 (1) 是硬断言，(2) 只做分类报告（见下面 classify）。
    Readback preRead;
    if (ok) {
        // 先读一次基准：这些区段在裸写之前是什么。它决定"OS 侧读到 A"该如何解读 ——
        // 若基准不是 A，说明文件系统的写根本没到磁盘（被上层过滤器截住了），
        // 那么 OS 侧读回 A 就完全在预期之内。
        std::vector<unsigned char> before;
        if (!ReadExtentsRaw(device, extents, kScratchSize, before, error)) {
            Print(FormatW(L"FAIL raw pre-read: %ls", error.c_str()));
            ok = false;
        } else {
            preRead = Summarize(before.data(), before.size());
            Print(FormatW(L"raw pre-read (before any raw write): %ls", preRead.Describe().c_str()));
            if (preRead.bytesA == before.size()) {
                Print(L"  -> the file-system write reached the disk; the extent mapping is exact");
            } else if (preRead.bytesB == before.size()) {
                Print(L"  -> these extents still hold 0x5A, i.e. what a previous run of this "
                      L"self-test wrote over the raw disk; the file's own content (0xA5) never "
                      L"reached them");
            } else {
                Print(L"  -> the file-system write did NOT reach the disk (a volume filter above "
                      L"the FS is holding it; a freeze/restore product does exactly this)");
            }
        }
    }

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

    if (ok) {
        // (1) 底层读回：必须读到 B —— 这是"驱动按这些物理偏移写进去了"的唯一判据。
        // （普通 ReadFile 会命中 FS 缓存里的旧页，看出"写了没生效"，其实写对了。）
        std::vector<unsigned char> rawRead;
        if (!ReadExtentsRaw(device, extents, kScratchSize, rawRead, error)) {
            Print(FormatW(L"FAIL raw read-back: %ls", error.c_str()));
            ok = false;
        } else {
            const Readback raw = Summarize(rawRead.data(), rawRead.size());
            if (raw.bytesB == rawRead.size()) {
                Print(L"raw read-back (driver path) matches pattern 0x5A: the write landed on disk");
            } else {
                Print(FormatW(L"FAIL raw read-back (driver path) does not match pattern 0x5A: %ls",
                              raw.Describe().c_str()));
                Print(L"     the driver path itself is broken (target disk, offsets or the write "
                      L"path) - this is a real failure, not a filter");
                ok = false;
            }
        }
    }

    if (ok) {
        // (2) OS 读回，非硬断言：见本节开头的两条通路说明。FILE_FLAG_NO_BUFFERING 要求
        // 缓冲区按扇区对齐（VirtualAlloc 给的是 64 KiB 对齐）且长度是扇区整数倍（64 KiB ✓）。
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
            const bool readOk =
                ::ReadFile(file, aligned, static_cast<DWORD>(kScratchSize), &got, nullptr) &&
                got == kScratchSize;
            ::CloseHandle(file);
            if (!readOk) {
                Print(FormatW(L"FAIL uncached read (sector=%u): %u", sector, ::GetLastError()));
                ok = false;
            } else {
                const auto* bytes = static_cast<const unsigned char*>(aligned);
                const Readback os = Summarize(bytes, kScratchSize);
                Print(FormatW(L"OS read-back (file-system path, uncached): %ls", os.Describe().c_str()));
                if (os.bytesB == kScratchSize) {
                    Print(L"  -> the file system sees what the driver wrote: nothing between the FS "
                          L"and the disk is shadowing this file");
                } else if (os.bytesA == kScratchSize) {
                    Print(L"  -> the file system still sees 0xA5: a volume filter is holding the "
                          L"original blocks. Expected on a frozen machine - the raw write below it "
                          L"succeeded anyway, which is exactly what melt relies on.");
                } else {
                    Print(L"FAIL the OS read-back is a mix (see the counts above), not a clean "
                          L"all-A or all-B result: the file's extents are shared with other data, "
                          L"or something modified the file while the test ran");
                    ok = false;
                }
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
    // 这一版不复位：写完并逐字节验证之后交回给操作者，由他自己重启（理由见 RunMelt 第 10 步）。
    opt.confirm = [](const MeltResult&) { return true; };

    Print(L"SecMelt melt (non-interactive; --yes-i-know was given)");
    Print(L"WARNING: this overwrites the SYSTEM hive on the raw disk and also its RegBack copy,");
    Print(L"         so no earlier hive generation survives on this machine. There is no rollback:");
    Print(L"         System Restore cannot undo it - only a VM snapshot taken beforehand can.");
    Print(L"No reset is triggered: once the writes verify, reboot the machine yourself with a normal");
    Print(L"restart - that is what applies the change. The log stays on screen until you do.");
    const MeltResult result = RunMelt(opt, [](const std::wstring& line) { Print(line); });
    if (!result.pending.empty()) {
        Print(L"--- planned actions ---");
        for (const auto& item : result.pending) Print(L"  " + item);
    }
    if (!result.ok) {
        // ok=true 只有两种来路：dryRun 预演跑完，或落盘路径写完且逐字节验证通过。
        // 所以"走到这里且 ok=false"就是中途失败（日志里已有原因）。
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

int PreflightScan(const std::filesystem::path& exeDir) {
    // --preflight: melt 的自动位置扫描入口。参数形状与 --melt 相同：不走 TUI
    // 的注册表编辑/hive 写回，不触发复位；唯一共享的粘合是：
    //   A) 驱动装载（可能 kdu -dse 0）
    //   B) 打开 handle: 设备做 raw I/O。
    // 输出与 melt 相同的行级日志，方便与 melt 同一番话术。
    MeltOptions opt;
    opt.dryRun = false;
    opt.exeDir = exeDir;
    opt.runNtfsFix = false;  // 这个入口里不走 ntfsfix：判读本来就围绕它的副作用展开
    opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";

    Print(L"SecMelt preflight scan (non-interactive)");
    Print(L"WARNING: this does raw disk I/O to the same system volume a melt would use;");
    Print(L"         it does NOT write SYSTEM/RegBack or reset, but it does load an unsigned");
    Print(L"         driver. Only meaningful on a VM with a snapshot.");
    const MeltResult result = RunPreflightScan(
        opt, [](const std::wstring& line) { Print(line); });
    Print(result.ok ? L"PREFLIGHT PASS" : L"PREFLIGHT FAIL (see scan lines above)");
    return result.ok ? 0 : 1;
}

}  // namespace secmelt
