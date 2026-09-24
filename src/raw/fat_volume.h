// IceMelt —— FatFs 的薄封装（裸扇区写 ESP 用的文件系统层）
//
// 为什么用 FatFs 而不是自己写：手写的那版是"每个簇一次系统调用"（FAT 表项逐条读、逐条写），
// 500 MB 的 ESP 上就是几十万到上百万次 I/O —— 实测把磁盘打到 85% 且长时间没有任何输出。
// FatFs（R0.15 w/patch1，third_party/FatFs，上游原文未改）按扇区/簇批量下发：整扇区直接读写、
// FAT 走滑窗，同一份活儿是几百次调用。
//
// 这层只做三件 FatFs 不管的事：
//   * 把 FatFs 的 diskio 接到 SectorIo（生产：PhysicalDriveSectorIo；自检：FileSectorIo）
//   * 写盘前把将被改写的扇区快照进 UndoJournal（--esp-undo 回放的基础）
//   * UTF-8 路径 → UTF-16、自动创建中间目录、按需读回校验
//
// 保持的语义（与原来的手写实现一致，调用方无需改动）：
//   * 路径用 UTF-8，分隔符 '\'；查找与新建都支持 LFN（\EFI\Microsoft 这类）
//   * 写文件的父目录自动创建
//   * 覆盖已有文件时旧簇链由 FatFs 释放（f_open 的 FA_CREATE_ALWAYS 走截断）
//   * 只接受 512 字节逻辑扇区（ESP 恒为 512）—— 非 512 直接报错，不做隐式换算

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "raw/block_io.h"

struct FatFsHandle; // 定义在 .cpp 里（内含 FatFs 的 FATFS 对象）—— 头文件不展开 ff.h，
                     // 也不做 `struct FATFS;` 这种前置声明：FatFs 的 FATFS 是**匿名结构体的
                     // typedef**，前置声明会与它冲突（实测：typedef redefinition）

namespace icemelt
{
	class FatFsVolume
	{
	public:
		FatFsVolume() = default;

		~FatFsVolume();

		FatFsVolume(const FatFsVolume &)            = delete;
		FatFsVolume &operator=(const FatFsVolume &) = delete;

		// 在 partitionBase 处挂载一个 FAT 卷（只读一个扇区确认几何，不动盘）
		bool Mount(SectorIo &io, uint64_t partitionBase, std::wstring &error);

		void Unmount();

		bool mounted() const
		{
			return mounted_;
		}

		// "FAT12" / "FAT16" / "FAT32"
		const wchar_t *type() const;

		uint64_t clusterSize() const; // 字节

		// 数据区容量（≈ 簇数 × 簇大小）；只用于日志
		uint64_t volumeBytes() const;

		// 空闲字节。首次现算（在 FatFs 窗口里扫 FAT），之后缓存；写操作会让缓存失效。
		uint64_t freeBytes();

		// path 用 UTF-8。返回 false 时**要看 error 是否为空**：
		//   * error 为空  = 确实不存在
		//   * error 非空  = 查询本身失败（读盘错误等），调用方不能用"不存在"处理
		bool Exists(const std::string &path, uint64_t *sizeOut, bool *isDir, std::wstring &error);

		// 递归创建目录（\EFI\IceMelt 的中间层不存在时一并创建）
		bool MakeDirectory(const std::string &path, std::wstring &error);

		// 流式写入：size 字节由 reader(offset, dst, len) 提供；父目录自动创建
		bool WriteFile(
			const std::string &                                   path,
			uint64_t                                              size,
			const std::function<bool(uint64_t, void *, size_t)> & reader,
			std::wstring &                                        error);

		bool ReadFile(const std::string &path, std::vector<uint8_t> &out, std::wstring &error);

		// 用同一视图读回并逐字节比较（reader 与 WriteFile 的语义相同）
		bool VerifyFile(
			const std::string &                                   path,
			uint64_t                                              size,
			const std::function<bool(uint64_t, void *, size_t)> & reader,
			std::wstring &                                        error);

		// 非空时，**所有**写入（数据 + FAT + 目录 + FSInfo）落盘前都先快照。
		// FatFs 在写数据的过程中就会更新 FAT，所以快照必须覆盖它改过的每个扇区。
		void SetJournal(UndoJournal *journal);

	private:
		bool  ensureDirectory(const std::string &dirUtf8, std::wstring &error);
		bool  makePath(const std::string &pathUtf8, std::wstring &out, std::wstring &error);
		void  refreshFree();

		int           pdrv_    = -1;
		FatFsHandle * handle_  = nullptr;
		bool          mounted_ = false;

		uint64_t freeBytes_ = 0;
		bool     freeValid_ = false;

		UndoJournal *journal_ = nullptr;
	};

	// 自检用：统计 diskio 层的调用次数与扇区数。
	//
	// 存在的理由是一次真实事故：手写的 FAT 写入器"每个簇一次系统调用"（FAT 表项逐条读、
	// freeBytes 逐簇扫），500 MB 的 ESP 上就是几十万到上百万次 I/O，现场表现是磁盘 85%、
	// 长时间没有任何输出。FatFs 按扇区/簇批量下发，同样的活儿只有几百次调用 —— 下面这对
	// 计数接口就是把这个差别写进回归测试，防止再退回去。
	struct FatIoStats
	{
		uint64_t reads        = 0;
		uint64_t writes       = 0;
		uint64_t readSectors  = 0;
		uint64_t writeSectors = 0;
	};

	void       EnableFatIoCounters(bool on);

	void       ResetFatIoCounters();

	FatIoStats FatIoCounters();

	// 自检用：把一个合成镜像格式化成 FAT32（内部就是 f_mkfs，FM_FAT32|FM_SFD）。
	// 整块镜像被当作"分区内容"格式化成空卷 —— 与真实 ESP 相同形态。
	bool FormatFat32ForTest(
		SectorIo &    io,
		uint64_t      base,
		uint64_t      sizeBytes,
		uint32_t      bytesPerSector,
		uint32_t      sectorsPerCluster,
		std::wstring &error);
} // namespace icemelt
