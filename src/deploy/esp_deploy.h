// IceMelt —— ESP 部署：把 pre-boot melt 链（UEFI Shell + ntfs-3g + startup.nsh +
// 打好补丁的 SYSTEM hive）铺进 EFI 系统分区
//
// 与 --melt 的分工：
//   --melt        走 WinDisk 裸盘写回，改写**当前运行中**这台机器的 SYSTEM hive；
//   --deploy-esp  只走普通文件路径：mountvol 挂出 ESP → CopyFileW → 卸载。它不改 hive
//                 内容，只负责把"重启动后由 UEFI Shell 脚本完成 melt"所需的文件就位。
//
// ESP 上的布局（与 gunpowder/startup.nsh 的约定一一对应，改一处必须同步另一处）：
//   \EFI\Microsoft\Boot\bootmgfw.efi   <- shellx64.efi（改名替换，默认 Boot#### 指它）
//   \EFI\Microsoft\Boot\startup.nsh    <- 自动执行脚本（Shell 规范 3.3.1：先找 shell 自身目录）
//   \EFI\IceMelt\ntfs-3g.efi           <- NTFS 驱动（RW 版，脚本靠它枚举 Windows 卷）
//   \EFI\IceMelt\SYSTEM                <- 现场生产的 payload：导出本机 HKLM\SYSTEM →
//                                          离线摘除名单里的服务键与设备类过滤器项 →
//                                          置干净/修校验和/可加载性校验 → 拷进 ESP
//   \EFI\IceMelt\bootmgfw-original.efi <- 原始 bootmgfw 备份（首次部署时从 ESP 现拷）
//
// 安全性质：
//   - 全程普通文件 API + 一次离线 hive 编辑（RegLoadKey 一份**副本**），不装载任何内核驱动，
//     不碰裸盘，也不改运行中的注册表（冻结机改它没有意义）；
//   - 原始 bootmgfw 备份只建一次（重复部署不会把"我们的 shell"当成原始文件备份）；
//   - 任何一步失败都保证卸载挂载点（RAII），不留占用的盘符。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace icemelt
{
	struct DeployResult
	{
		bool                      ok = false;
		std::vector<std::wstring> log; // 全量日志（ASCII，行首 [+]/[x]/[!] 供 CliPaint 分层）
	};

	// 把 <exeDir>\gunpowder 下的资产部署到本机 ESP。
	// 需要管理员令牌（mountvol /S 与写 ESP 的硬前提）；缺资产时挂载前即失败。
	DeployResult DeployEsp(const std::filesystem::path &exeDir);
} // namespace icemelt
