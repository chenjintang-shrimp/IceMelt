#include "reg/hive.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <vector>

#include "util.h"

namespace secmelt {
namespace {

// base block 校验和：前 kHiveChecksumOffset 字节按 ULONG 逐字异或，再做规范要求的
// 归一化（msuhanov/regf 注 2）：结果等于 0xFFFFFFFF 时取 0xFFFFFFFE，等于 0 时取 1。
// 少了这两步，恰好落在特例上的 hive 会被内核判成校验和无效 —— 也就是「脏」，
// 于是触发日志恢复，反而把我们要消除的那条路径打开了。
// 算法正确性由 --selftest-hive 用真实 Windows 产出的 hive 判定（stored == computed）。
uint32_t ComputeHiveChecksum(const unsigned char* block) {
    uint32_t checksum = 0;
    for (uint32_t offset = 0; offset < kHiveChecksumOffset; offset += sizeof(uint32_t)) {
        uint32_t word = 0;
        std::memcpy(&word, block + offset, sizeof(word));
        checksum ^= word;
    }
    if (checksum == 0xFFFFFFFFu) checksum = 0xFFFFFFFEu;
    if (checksum == 0) checksum = 1;
    return checksum;
}


uint32_t ReadU32(const unsigned char* block, uint32_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, block + offset, sizeof(value));
    return value;
}

void WriteU32(unsigned char* block, uint32_t offset, uint32_t value) {
    std::memcpy(block + offset, &value, sizeof(value));
}

bool ReadBlock(const std::filesystem::path& hivePath, std::vector<unsigned char>& block,
               uint64_t& fileSize, std::wstring& error) {
    std::ifstream in(hivePath, std::ios::binary | std::ios::ate);
    if (!in) {
        error = FormatW(L"cannot open hive for read: %ls", hivePath.c_str());
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size < static_cast<std::streamoff>(kHiveBlockSize)) {
        error = FormatW(L"hive is smaller than one base block (%lld bytes)", static_cast<long long>(size));
        return false;
    }
    block.assign(kHiveBlockSize, 0);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(kHiveBlockSize))) {
        error = FormatW(L"cannot read base block from: %ls", hivePath.c_str());
        return false;
    }
    fileSize = static_cast<uint64_t>(size);
    return true;
}

// 从一段至少 kHiveBlockSize 字节的内存解析 base block（零拷贝：缓冲区由调用方持有）
void FillBaseBlock(const unsigned char* block, uint64_t fileSize, HiveBaseBlock& out) {
    out = HiveBaseBlock{};
    out.fileSize = fileSize;
    out.signatureOk = std::memcmp(block, "regf", 4) == 0;
    out.sequence1 = ReadU32(block, kHiveSequence1Offset);
    out.sequence2 = ReadU32(block, kHiveSequence2Offset);
    out.major = ReadU32(block, kHiveMajorOffset);
    out.minor = ReadU32(block, kHiveMinorOffset);
    out.type = ReadU32(block, kHiveTypeOffset);
    out.format = ReadU32(block, kHiveFormatOffset);
    out.rootCell = ReadU32(block, kHiveRootCellOffset);
    out.binsSize = ReadU32(block, kHiveBinsSizeOffset);
    out.clusteringFactor = ReadU32(block, kHiveClusteringFactorOffset);
    out.storedChecksum = ReadU32(block, kHiveChecksumOffset);
    out.computedChecksum = ComputeHiveChecksum(block);
    out.clean = out.sequence1 == out.sequence2;
    out.checksumOk = out.storedChecksum == out.computedChecksum;
    // root cell 必须指向文件内一个完整的 cell 头 + "nk" 签名
    out.rootCellValid = RootCellDataOffset(out) + 2 <= fileSize;
}

}  // namespace

uint64_t RootCellDataOffset(const HiveBaseBlock& base) {
    return kHiveBlockSize + base.rootCell + sizeof(int32_t);
}

bool ParseBaseBlock(const unsigned char* block, size_t blockSize, uint64_t fileSize,
                    HiveBaseBlock& out) {
    if (!block || blockSize < kHiveBlockSize) return false;
    FillBaseBlock(block, fileSize, out);
    return true;
}

bool ExportSystemHive(const std::filesystem::path& outPath, std::wstring& error) {
    std::error_code ec;
    secmelt::RemoveFile(outPath);  // RegSaveKeyEx 在目标已存在时失败

    std::wstring privilegeError;
    if (!EnablePrivilege(SE_BACKUP_NAME, privilegeError)) {
        error = FormatW(L"SE_BACKUP_NAME: %ls", privilegeError.c_str());
        return false;
    }

    // RegSaveKeyEx 保存的是「句柄所指的那个键」，因此必须先打开 HKLM\SYSTEM 子键，
    // 直接把 HKEY_LOCAL_MACHINE 传进去只会去保存整个 HKLM。
    HKEY systemKey = nullptr;
    LSTATUS status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM", 0, KEY_READ, &systemKey);
    if (status != ERROR_SUCCESS) {
        error = FormatW(L"RegOpenKeyExW(HKLM\\SYSTEM) failed: %ld", status);
        return false;
    }

    // winreg.h: RegSaveKeyExW(hKey, lpFile, lpSecurityAttributes, flags)
    status = ::RegSaveKeyExW(systemKey, outPath.c_str(), nullptr, REG_LATEST_FORMAT);
    ::RegCloseKey(systemKey);
    if (status != ERROR_SUCCESS) {
        error = FormatW(L"RegSaveKeyExW(SYSTEM) failed: %ld (target: %ls)", status, outPath.c_str());
        return false;
    }
    if (!secmelt::PathIsRegularFile(outPath)) {
        error = FormatW(L"RegSaveKeyExW reported success but %ls is missing", outPath.c_str());
        return false;
    }
    return true;
}

bool ReadBaseBlock(const std::filesystem::path& hivePath, HiveBaseBlock& out, std::wstring& error) {
    std::vector<unsigned char> block;
    uint64_t fileSize = 0;
    if (!ReadBlock(hivePath, block, fileSize, error)) return false;
    FillBaseBlock(block.data(), fileSize, out);
    return true;
}

bool MakeCleanAndFixChecksum(const std::filesystem::path& hivePath, HiveBaseBlock& out,
                             std::wstring& error) {
    std::vector<unsigned char> block;
    uint64_t fileSize = 0;
    if (!ReadBlock(hivePath, block, fileSize, error)) return false;

    if (fileSize % kHiveBlockSize != 0) {
        error = FormatW(L"hive size %llu is not a multiple of %llu", fileSize, kHiveBlockSize);
        return false;
    }
    const uint32_t type = ReadU32(block.data(), kHiveTypeOffset);
    if (type != 0) {
        error = FormatW(L"hive file type is %u, expected 0 (primary)", type);
        return false;
    }

    const uint32_t sequence1 = ReadU32(block.data(), kHiveSequence1Offset);
    // (b) 干净态：seq2 = seq1。RegSaveKeyEx 正常已产出干净 hive，这一步通常无改动。
    WriteU32(block.data(), kHiveSequence2Offset, sequence1);
    // (c) 重算校验和
    const uint32_t checksum = ComputeHiveChecksum(block.data());
    WriteU32(block.data(), kHiveChecksumOffset, checksum);

    std::fstream file(hivePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        error = FormatW(L"cannot reopen hive for write: %ls", hivePath.c_str());
        return false;
    }
    file.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(kHiveBlockSize));
    file.flush();
    if (!file) {
        error = FormatW(L"cannot write base block to: %ls", hivePath.c_str());
        return false;
    }

    return ReadBaseBlock(hivePath, out, error);
}

bool VerifyHiveLoadable(const std::filesystem::path& hivePath, std::wstring& error) {
    HiveBaseBlock base;
    if (!ReadBaseBlock(hivePath, base, error)) return false;

    if (!base.signatureOk) {
        error = L"hive base block signature is not \"regf\"";
        return false;
    }
    if (base.type != 0) {
        error = FormatW(L"hive file type is %u, expected 0 (primary)", base.type);
        return false;
    }
    if (base.fileSize % kHiveBlockSize != 0) {
        error = FormatW(L"hive size %llu is not a multiple of %llu", base.fileSize, kHiveBlockSize);
        return false;
    }
    if (!base.clean) {
        error = FormatW(L"hive is dirty (sequence1=%u, sequence2=%u)", base.sequence1, base.sequence2);
        return false;
    }
    if (!base.checksumOk) {
        error = FormatW(L"hive checksum mismatch (stored=0x%08X computed=0x%08X)", base.storedChecksum,
                        base.computedChecksum);
        return false;
    }
    if (!base.rootCellValid) {
        error = FormatW(L"root cell offset %u is outside the hive bins data area (size %u)", base.rootCell,
                        base.binsSize);
        return false;
    }
    // root cell 单元必须以 "nk" 开头：这一步能抓住「文件被截断/内容不是 hive」的情况
    std::ifstream in(hivePath, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(RootCellDataOffset(base)));
    char signature[2] = {};
    if (!in.read(signature, 2) || std::memcmp(signature, "nk", 2) != 0) {
        error = FormatW(L"root cell at offset %u is not an \"nk\" cell", base.rootCell);
        return false;
    }
    return true;
}

namespace {

// 离线加载导出副本时用的临时键名（只在函数内存在，异常路径也会卸载）
constexpr const wchar_t* kOfflineKeyName = L"SECMELT_OFFLINE";

// 在**已加载**的离线 hive 里，按 Select\Current（兼顾 Default）遍历活动 ControlSet，
// 删掉 <ControlSet>\Services\<name>。返回是否删掉了至少一个。
bool DeleteServiceInControlSets(HKEY offlineRoot, const std::wstring& serviceName, bool& deleted,
                                std::wstring& error) {
    std::vector<std::wstring> sets;
    for (const wchar_t* which : {L"Current", L"Default"}) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (::RegGetValueW(offlineRoot, L"Select", which, RRF_RT_REG_DWORD, &type, &value, &size) ==
            ERROR_SUCCESS) {
            const std::wstring name = FormatW(L"ControlSet%03u", value);
            bool seen = false;
            for (const auto& existing : sets) {
                if (existing == name) seen = true;
            }
            if (!seen) sets.push_back(name);
        }
    }
    if (sets.empty()) {
        error = L"the hive has no Select\\Current: cannot tell which ControlSet is active";
        return false;
    }

    for (const auto& set : sets) {
        const std::wstring path = FormatW(L"%ls\\Services\\%ls", set.c_str(), serviceName.c_str());
        const LSTATUS status = ::RegDeleteTreeW(offlineRoot, path.c_str());
        if (status == ERROR_SUCCESS) {
            deleted = true;
        } else if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
            error = FormatW(L"RegDeleteTreeW(%ls) failed: %ld", path.c_str(), status);
            return false;
        }
    }
    return true;
}

}  // namespace

bool RemoveServiceFromExportedHive(const std::filesystem::path& hivePath,
                                   const std::wstring& serviceName, std::wstring& error) {
    std::wstring privilegeError;
    if (!EnablePrivilege(SE_BACKUP_NAME, privilegeError)) {
        error = FormatW(L"SE_BACKUP_NAME: %ls", privilegeError.c_str());
        return false;
    }
    if (!EnablePrivilege(SE_RESTORE_NAME, privilegeError)) {
        error = FormatW(L"SE_RESTORE_NAME: %ls", privilegeError.c_str());
        return false;
    }

    // 摘掉可能残留的同名键（上一次异常中断留下的）
    ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kOfflineKeyName);

    const LSTATUS load = ::RegLoadKeyW(HKEY_LOCAL_MACHINE, kOfflineKeyName, hivePath.c_str());
    if (load != ERROR_SUCCESS) {
        error = FormatW(L"RegLoadKeyW(%ls) failed: %ld", hivePath.c_str(), load);
        return false;
    }

    bool ok = false;
    bool deleted = false;
    do {
        HKEY root = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kOfflineKeyName, 0, KEY_READ | KEY_WRITE, &root) !=
            ERROR_SUCCESS) {
            error = FormatW(L"cannot open the loaded hive %ls", kOfflineKeyName);
            break;
        }
        ok = DeleteServiceInControlSets(root, serviceName, deleted, error);
        ::RegCloseKey(root);
    } while (false);

    // 无论成败都要卸载：留着会让 hive 处于半写状态，也会让下次 RegLoadKey 失败
    const LSTATUS unload = ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kOfflineKeyName);
    if (unload != ERROR_SUCCESS) {
        if (ok) error = FormatW(L"RegUnLoadKeyW(%ls) failed: %ld", kOfflineKeyName, unload);
        return false;
    }
    (void)deleted;
    return ok;
}

}  // namespace secmelt
