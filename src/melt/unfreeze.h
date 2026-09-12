// SecMelt —— “解冻”编排
//
// 顺序（每步失败即中止，保留已完成步骤的日志）：
//   1 预检：提权 / DSE 已关 / WinDisk 驱动文件存在
//   2 装载驱动   3 卷信息 + 切换目标磁盘   4 加载名单
//   5 注册表摘除（dryRun 时改为只读探测）
//   6 导出 hive → 读 base block → 置干净并修校验和 → 可加载性校验
//   7 用户确认（仅非 dryRun；此前尚未发生任何裸盘写入）
//   8 写回 hive（可选 ntfsfix → ntfscp）
//   9 清零 SYSTEM.LOG1/.LOG2 头部 0x1000 字节并读回断言
//  10 硬重启（此步之前不再有任何提示或等待）
//
// 第 7 步与第 10 步之间保证无用户交互：confirm 为空时非 dryRun 直接中止，
// 拒绝在没有确认回调的情况下落盘。

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace secmelt {

struct MeltOptions {
    bool dryRun = false;
    bool runNtfsFix = true;
    std::filesystem::path winDiskSysPath;   // 缺省 <exeDir>/WinDisk_x64.sys
    std::filesystem::path hiveOutPath;      // 缺省 %TEMP%\secmelt-system.hive
    std::filesystem::path exeDir;           // 缺省 ExeDir()
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
                                        // 将写入的 hive 路径与大小、将清零的两个日志文件
};

using MeltLogger = std::function<void(const std::wstring&)>;

// 执行整条链路。log 可为空；只读阶段（dryRun / 预检 / 第 6 步）不需要提权以外的条件。
MeltResult RunMelt(const MeltOptions& opt, const MeltLogger& log);

// 供 TUI 复用的只读预演（等价于 RunMelt(opt{dryRun=true})）
MeltResult DryRun(const MeltOptions& opt, const MeltLogger& log);

}  // namespace secmelt
