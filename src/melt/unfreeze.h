// SecMelt —— “解冻”编排
//
// 顺序（每步失败即中止，保留已完成步骤的日志）：
//   1 预检：提权 / WinDisk 驱动就位
//   2 装载驱动（被 577 拒绝 = 签名强制还在拦 → kdu -dse 0 → 重新装载复测）
//   3 卷信息 + 切换目标磁盘
//   4 加载名单
//   5 注册表摘除（dryRun 时改为只读探测）
//   6 导出 hive → 从副本里离线摘掉装载用的服务键 → 置干净并修校验和 → 可加载性校验
//   7 用户确认（仅非 dryRun；此前尚未发生任何裸盘写入）
//   8 写回 hive：写前守卫（ntfs-3g 视图确认目标是 regf hive）→ 可选 ntfsfix → ntfscp
//     → 经 ntfs-3g 读回逐字节校验 + base block 断言
//   8b 同一份 hive 也写进 config\RegBack\SYSTEM（若存在），让主/备代数一致
//   9 只读检查 SYSTEM.LOG / .LOG1 / .LOG2（**不改动**：清零在干净 hive 下不改变任何结果）
//  10 置系统卷的 dirty 标记（autochk 下次启动即检查它）→ 硬重启
//
// 第 7 步与第 10 步之间保证无用户交互：confirm 为空时非 dryRun 直接中止，
// 拒绝在没有确认回调的情况下落盘。

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "raw/win_disk.h"  // DriverLoad

namespace secmelt {

// 「未签名的 WinDisk 能不能装载」这一次探测的完整结果。
//
// 判据只有一个 —— 装载返回码（见 win_disk.h::DriverLoad）：577 就是签名强制还在拦。
// 被拦下时**不是**去换个接口"查 DSE"，而是直接 kdu -dse 0，然后**重新装载一次**：
// 装上了才算真的解开。kdu 自己的退出码不作数（它的 ControlDSE 回调在"值本来就是 0"时
// 也返回 1，实测）。
struct DriverLoadOutcome {
    DriverLoad result = DriverLoad::Failed;  // 最终的装载结果
    bool kduRan = false;                     // 这次是否真的调用过 kdu
    std::wstring firstError;                 // 第一次装载失败的原因（空 = 没失败）
    std::wstring kduLog;                     // kdu 的原始输出（空 = 没调用）
    std::wstring retryError;                 // 重新装载失败的原因（空 = 没失败或已成功）
};

// 确保「未签名的 WinDisk 能被装载」这件事成立：
//   装载 → 被 577(ERROR_INVALID_IMAGE_HASH) 拒绝 → kdu -dse 0 → 重新装载 → 复测结果。
//
// melt 的第一步和环境自检共用这一份实现，免得两处各写一遍然后漂移（环境自检原先只报
// "melt runs kdu -dse 0 and retries"，那是预测不是结论）。
//
// allowKdu=false：只探测、不调用 kdu（用于 --dump 这类必须保持"只读"的纯报告路径）。
// 探测失败时会顺手删掉本次探测建出来的服务键 —— 前提正是"装载被拒"，没有驱动在跑，
// 注册表与 SCM 因此一致；反过来装载成功时**绝不能删**（驱动还加载着，删键会让两者脱节）。
DriverLoadOutcome EnsureUnsignedDriverLoads(const std::filesystem::path& exeDir, bool allowKdu);

struct MeltOptions {
    bool dryRun = false;
    bool runNtfsFix = true;
    std::filesystem::path winDiskSysPath;   // 缺省 <exeDir>/WinDisk_x64.sys
    std::filesystem::path hiveOutPath;      // 缺省 %TEMP%\secmelt-system.hive
    std::filesystem::path exeDir;           // 缺省 ExeDir()
    // 第 10 步的复位是否由人手动触发（而不是写完就自动 bugcheck）。
    //
    // 用途：自动复位会立刻把机器打下去，操作者来不及看日志、也来不及用别的工具核对现场。
    // CLI 模式下因此默认等一次回车再触发。
    //
    // **等待是有代价的**：写完裸盘之后，内存里那份注册表迟早会被懒写回覆盖磁盘上刚写好的
    // hive。所以这是"给你一点时间看清楚"，不是"可以慢慢来"。
    bool manualBugcheck = false;
    // 非 dryRun 时必需：收到「待确认」清单后返回是否继续。为空即中止。
    std::function<bool(const struct MeltResult&)> confirm;
};

struct MeltResult {
    bool ok = false;
    // 是否已经动过裸盘。失败时调用方据此区分「什么都没发生」与「状态可能不一致」——
    // 这两种情况给用户的指示完全不同。
    bool wroteDisk = false;
    std::vector<std::wstring> log;      // 全量日志（ASCII）
    std::vector<std::wstring> pending;  // 待用户确认的动作（仅第 7 步）：
                                        // 将写入的 hive 与其 RegBack 副本、将置的卷标记、复位
};

using MeltLogger = std::function<void(const std::wstring&)>;

// 执行整条链路。log 可为空；只读阶段（dryRun / 预检 / 第 6 步）不需要提权以外的条件。
MeltResult RunMelt(const MeltOptions& opt, const MeltLogger& log);

// 供 TUI 复用的只读预演（等价于 RunMelt(opt{dryRun=true})）
MeltResult DryRun(const MeltOptions& opt, const MeltLogger& log);

// --preflight 一次性位置扫描：装驱动 + 打开设备后跳过注册表与 hive 写回，只做
// logstate + 四深度目录对照写/校验/取证 + 失败时三级 DiagnoseWritePath 拆解。
// 不写 SYSTEM、不碰 RegBack、不复位；结束卸载并删除 WinDisk。
MeltResult RunPreflightScan(const MeltOptions& opt, const MeltLogger& log);

}  // namespace secmelt
