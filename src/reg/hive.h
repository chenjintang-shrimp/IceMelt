// SecMelt —— SYSTEM hive 导出与 base block 校验/修补
//
// regf base block（公开格式规范 msuhanov/regf；字段偏移在本文件以常量形式集中定义，
// 由 --selftest-hive 用**真实 Windows 产出的 hive** 判定其正确性）：
//
//   0x000  4  signature "regf"
//   0x004  4  sequence1         主序号
//   0x008  4  sequence2         次序号（与 sequence1 不等即为「脏」，会触发日志恢复）
//   0x00C  8  last written timestamp
//   0x014  4  major version
//   0x018  4  minor version
//   0x01C  4  file type         (0 = primary hive，1 = transaction log)
//   0x020  4  file format       (1 = direct memory load)
//   0x024  4  root cell offset  (hive bins 数据区内相对偏移)
//   0x028  4  hive bins data size
//   0x02C  4  clustering factor
//   0x030 64  file name (UTF-16)
//   0x1FC  4  checksum = 前 0x1FC 字节按 ULONG 逐字异或
//
// 恢复语义：只有「主 hive 脏 或 校验和无效」时内核才会应用事务日志；日志项又必须从
// 期望序号起连续。因此 (a) 写回干净且校验和有效的 hive，(b) 清零日志头部，
// 两条独立防线共同消除日志重放把我们的改动覆盖回去的可能。

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace secmelt {

// 偏移常量（唯一定义源；实现与自检都从这里取）
constexpr uint64_t kHiveBlockSize = 4096;         // 基础块 / 对齐单位
constexpr uint32_t kHiveChecksumOffset = 0x1FC;   // 校验和字段偏移
constexpr uint32_t kHiveSequence1Offset = 0x04;
constexpr uint32_t kHiveSequence2Offset = 0x08;
constexpr uint32_t kHiveMajorOffset = 0x14;
constexpr uint32_t kHiveMinorOffset = 0x18;
constexpr uint32_t kHiveTypeOffset = 0x1C;
constexpr uint32_t kHiveFormatOffset = 0x20;
constexpr uint32_t kHiveRootCellOffset = 0x24;
constexpr uint32_t kHiveBinsSizeOffset = 0x28;
constexpr uint32_t kHiveClusteringFactorOffset = 0x2C;

struct HiveBaseBlock {
    bool signatureOk = false;      // offset 0 为 "regf"
    uint32_t sequence1 = 0;        // 0x04
    uint32_t sequence2 = 0;        // 0x08
    uint32_t major = 0;            // 0x14
    uint32_t minor = 0;            // 0x18
    uint32_t type = 0;             // 0x1C: 0 = primary
    uint32_t format = 0;           // 0x20
    uint32_t rootCell = 0;         // 0x24
    uint32_t binsSize = 0;         // 0x28
    uint32_t clusteringFactor = 0; // 0x2C
    uint32_t storedChecksum = 0;   // 0x1FC 中的值
    uint32_t computedChecksum = 0; // 本实现重算出的值
    uint64_t fileSize = 0;         // 文件实际长度（非 base block 字段）
    bool rootCellValid = false;    // root cell 落在 bins 数据区内且签名为 "nk"
    bool clean = false;            // sequence1 == sequence2
    bool checksumOk = false;       // storedChecksum == computedChecksum
};

// root cell 单元的 "nk" 偏移。
// 0x24 处的值相对「第一个 hive bin 的起始」（即 base block 之后的 0x1000）计，
// 指向 cell 头；cell 头是 4 字节尺寸字段，"nk" 签名在其后。
// 该规则由 --selftest-hive 用真实 Windows 产出的 hive 判定（见 selftest.cpp）。
uint64_t RootCellDataOffset(const HiveBaseBlock& base);

// 导出 HKLM\SYSTEM 到 outPath（REG_LATEST_FORMAT，SE_BACKUP_NAME）。
// RegSaveKeyEx 在目标文件已存在时失败，故先删除已存在的目标文件。
bool ExportSystemHive(const std::filesystem::path& outPath, std::wstring& error);

// 读取并解析 base block（只读）
bool ReadBaseBlock(const std::filesystem::path& hivePath, HiveBaseBlock& out, std::wstring& error);

// 令 hive 变干净：sequence2 = sequence1，重算校验和并写回。
// 断言 size % 4096 == 0 且 type == 0。RegSaveKeyEx 正常已产出干净 hive，
// 因此这三步以校验为主、修正为辅。调用方用调用前后的 base block 比对得知是否真做了修正。
bool MakeCleanAndFixChecksum(const std::filesystem::path& hivePath, HiveBaseBlock& out,
                             std::wstring& error);

// 结构可加载性校验（不触碰运行中的注册表，不做 RegLoadKey）：
// signature / type / 对齐 / 序号干净 / 校验和有效 / root cell 为合法 "nk" 单元。
bool VerifyHiveLoadable(const std::filesystem::path& hivePath, std::wstring& error);

}  // namespace secmelt
