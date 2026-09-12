#include "reg/hive.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <vector>

#include "util.h"

namespace secmelt {
namespace {

// base block 校验和：前 kHiveChecksumOffset 字节按 ULONG 逐字异或。
// 算法正确性由 --selftest-hive 用真实 Windows 产出的 hive 判定（stored == computed）。
uint32_t ComputeHiveChecksum(const unsigned char* block) {
    uint32_t checksum = 0;
    for (uint32_t offset = 0; offset < kHiveChecksumOffset; offset += sizeof(uint32_t)) {
        uint32_t word = 0;
        std::memcpy(&word, block + offset, sizeof(word));
        checksum ^= word;
    }
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

void FillBaseBlock(const std::vector<unsigned char>& block, uint64_t fileSize, HiveBaseBlock& out) {
    out = HiveBaseBlock{};
    out.fileSize = fileSize;
    out.signatureOk = std::memcmp(block.data(), "regf", 4) == 0;
    out.sequence1 = ReadU32(block.data(), kHiveSequence1Offset);
    out.sequence2 = ReadU32(block.data(), kHiveSequence2Offset);
    out.major = ReadU32(block.data(), kHiveMajorOffset);
    out.minor = ReadU32(block.data(), kHiveMinorOffset);
    out.type = ReadU32(block.data(), kHiveTypeOffset);
    out.format = ReadU32(block.data(), kHiveFormatOffset);
    out.rootCell = ReadU32(block.data(), kHiveRootCellOffset);
    out.binsSize = ReadU32(block.data(), kHiveBinsSizeOffset);
    out.clusteringFactor = ReadU32(block.data(), kHiveClusteringFactorOffset);
    out.storedChecksum = ReadU32(block.data(), kHiveChecksumOffset);
    out.computedChecksum = ComputeHiveChecksum(block.data());
    out.clean = out.sequence1 == out.sequence2;
    out.checksumOk = out.storedChecksum == out.computedChecksum;
    // root cell 必须指向文件内一个完整的 cell 头 + "nk" 签名
    out.rootCellValid = RootCellDataOffset(out) + 2 <= fileSize;
}

}  // namespace

uint64_t RootCellDataOffset(const HiveBaseBlock& base) {
    return kHiveBlockSize + base.rootCell + sizeof(int32_t);
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
    FillBaseBlock(block, fileSize, out);
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

}  // namespace secmelt
