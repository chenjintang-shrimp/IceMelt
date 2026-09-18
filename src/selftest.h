// SecMelt —— 无交互自检（CLI）
//
// 三个开关都以纯 ASCII 打到 stdout，退出码 0 = 全部断言成立：
//   --selftest-hive   导出 SYSTEM hive，打印 base block 全部字段并断言
//                     signatureOk / computedChecksum == storedChecksum / sequence1 == sequence2 /
//                     fileSize % 4096 == 0
//   --selftest-raw    普通 API 建 64 KiB 图案 A 文件 → 载驱动 → 卷信息 → 物理区段 →
//                     按段写图案 B → 经驱动读回断言全 B（硬断言）→ 再经文件系统无缓冲
//                     读回并分类报告 → 删文件。
//                     两条读回通路的意义不同：驱动那条证明"写落到了磁盘上"，文件系统
//                     那条反映"这台机器上文件系统看到什么"。冻结状态下两者必然不一致
//                     （卷过滤器在上层截住写），那是过滤器在工作的证据，不是失败。
//   --dry-run         预检 + 只读注册表探测 + 导出/校验 hive；不做任何裸盘写入、不重启
//   --selftest-registry 用 scratch 键验证过滤器摘除的写入路径（字符串列表重写 / 整值删除）
//   --melt --yes-i-know 非交互执行整条链路（不需要 VT 终端；破坏性，必须显式确认）
//
// 后两个会装载内核驱动 / 写裸盘，只能在允许这类操作的机器（虚拟机）上运行。
// --selftest-registry 只写 HKLM\SOFTWARE\SecMelt\Selftest 下的临时键，不碰设备类键。

#pragma once

#include <filesystem>

namespace secmelt {

// --melt        非交互执行整条链路（等价于 TUI 里的 Melt，不需要能渲染 VT 的终端）。
//               必须同时给 --yes-i-know：落盘不可逆，这个 token 就是"确认"本身。
int MeltApply(const std::filesystem::path& exeDir, bool runNtfsFix);

// --preflight   melt 的一次性位置扫描：同一份字节写入卷根 → \Windows →
//               \Windows\System32 → \Windows\System32\config 并读回校验，
//               再附 DiagnoseWritePath 三级拆解与 logstate 守卫。
//               不写 SYSTEM、不碰 RegBack、不复位；写回导出没变。
int PreflightScan(const std::filesystem::path& exeDir);
int HiveSelfTest(const std::filesystem::path& exeDir);
int RegistrySelfTest(const std::filesystem::path& exeDir);
int RawSelfTest(const std::filesystem::path& exeDir);
int MeltDryRun(const std::filesystem::path& exeDir, bool runNtfsFix);

}  // namespace secmelt
