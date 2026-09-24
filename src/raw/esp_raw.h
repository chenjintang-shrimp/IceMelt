// IceMelt —— 用**普通物理盘句柄**（\\.\PhysicalDriveN）定位并改写 ESP 的文件
//
// 与 melt 的裸盘路径的区别：那条走自研的 WinDisk 驱动（端口驱动层，绕开卷过滤器）；
// 这条走 Windows 自己的 disk.sys 栈 —— 不需要装载未签名驱动、不需要解开 DSE，只需要
// 管理员令牌。能不能绕过还原软件取决于过滤器挂在哪一层：
//   * DfDiskLo（sector 层）只影子 MBR/GPT 那几个扇区，数据区经 hook stub 原样直通；
//   * DeepFrz（卷/文件层）的 COW 看不到不经卷栈的裸扇区写。
// 所以"写 ESP 的 FAT 数据区"这条路成立，前提是**不碰分区表**（本实现的读取只用来看布局，
// 写入只落在 FAT 元数据与文件数据上）。
//
// ESP 是 FAT16/FAT32，而裸扇区写没有文件系统概念 —— 文件操作由 raw/fat.h 的最小 FAT
// 写入器承担。

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace icemelt
{
	class PhysicalDriveSectorIo;

	struct EspPartition
	{
		uint32_t diskNumber  = 0;
		uint64_t offsetBytes = 0;
		uint64_t lengthBytes = 0;
		bool     gpt         = false;
		std::wstring description; // "GPT partition 1 (EFI System Partition)" 之类，用于日志
	};

	// 扫描 0..maxDisk 号物理盘，列出其中的 EFI 系统分区（只读）。
	// 需要管理员令牌：打开物理盘做读操作也要提权。
	bool FindEspPartitions(
		uint32_t                   maxDisk,
		std::vector<EspPartition> &out,
		std::wstring &             error);

	// 挑一个 ESP：若宿主的系统卷所在磁盘上恰好有一个 ESP，就用它；否则必须恰好只有一个
	// 候选，多了就得由调用方指定（避免"替你挑一块盘写引导"）。
	bool SelectEspPartition(
		const std::vector<EspPartition> &candidates,
		EspPartition &                   chosen,
		std::vector<std::wstring> &      notes,
		std::wstring &                   error);
} // namespace icemelt
