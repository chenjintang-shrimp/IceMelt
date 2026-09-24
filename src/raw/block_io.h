// IceMelt —— 扇区 I/O 抽象与写前快照
//
// 这一层与 FAT 无关：它只管"以磁盘绝对字节偏移读写扇区"，以及"写之前把将被改写的扇区
// 快照下来以便回放"。文件系统那层已经换成 FatFs（third_party/FatFs，见 raw/fat_volume.h），
// 这里的东西是 FatFs 碰不到、必须自己写的：
//
//   * SectorIo            —— FatFs 的 disk_read/disk_write 最终落到这里
//   * FileSectorIo        —— 把普通文件当扇区设备（自检：合成镜像）
//   * PhysicalDriveSectorIo —— \\.\PhysicalDriveN（走 Windows 自己的 disk.sys 栈，
//                              不装驱动、不看卷过滤器；DeepFrz 挂在卷层、没有 SRB 能力，
//                              所以经端口层的裸写不在它视野里；会被 DfDiskLo 吞掉的只有
//                              MBR/GPT 那几个扇区，而本工具不碰它们）
//   * UndoJournal         —— 元数据扇区快照/回放
//   * LockCoveringVolume  —— 裸写"已挂载文件系统覆盖的扇区"会被 Windows 拒绝(5)，
//                              所以要先对该卷 FSCTL_LOCK_VOLUME / FSCTL_DISMOUNT_VOLUME
//
// 为什么不用 Windows 的 FAT 驱动：卷栈正是还原软件拦截的那一层（见 README 拦截层级表）。

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace icemelt
{
	// 扇区读写抽象。实现者负责"偏移是磁盘/镜像绝对字节偏移"这一语义。
	class SectorIo
	{
	public:
		virtual ~SectorIo() = default;

		virtual bool Read(uint64_t offset, void *dst, size_t len) = 0;

		virtual bool Write(uint64_t offset, const void *src, size_t len) = 0;

		// 最近一次失败的可读原因（含 Win32 错误码）。默认空，实现者应尽量填 ——
		// 裸写被内核拒绝时，这是唯一能说清"为什么"的东西：Windows 对**已挂载且未锁定**
		// 的卷覆盖的扇区直接返回 ERROR_ACCESS_DENIED(5)，表现形式就是一次失败的写盘。
		virtual std::wstring failure() const
		{
			return {};
		}
	};

	// 写前快照。按扇区（512 字节粒度，对齐）记录原内容，每个扇区只记一次；回放即可把
	// **元数据**恢复原样（文件因此消失、簇回到空闲池）。
	//
	// 注意它能恢复什么、不能恢复什么：FAT/FSInfo/目录条目会回到写之前；被写过的空闲簇里
	// 残留我们的数据（对 FAT 语义无影响 —— 空闲簇本来就可以含任意垃圾）。我们只动 FAT 表项
	// 为 0 的簇，所以不会破坏任何现存文件的内容。
	class UndoJournal
	{
	public:
		// 记录 [offset, offset+len) 覆盖到的所有 512 字节扇区（已记录过的跳过）。
		// io 读失败即返回 false —— 快照不完整时调用方必须放弃这次写入。
		bool Capture(SectorIo &io, uint64_t offset, size_t len, std::wstring &error);

		// 落盘为一个文件：魔数 + 版本 + 磁盘号 + 条目表（偏移 + 长度 + 原始字节）。
		// 磁盘号必须存：偏移是**磁盘绝对字节偏移**，回放时得知道往哪块盘写。
		bool SaveTo(const std::filesystem::path &file, uint32_t diskNumber, std::wstring &error) const;

		// 只读取出文件里记的磁盘号（--esp-undo 需要先知道对哪块盘下手）
		static bool ReadDiskNumber(const std::filesystem::path &file, uint32_t &diskNumber, std::wstring &error);

		// 反向操作：把文件里记录的原始字节写回去（从后往前，重叠区先写的会被后写的覆盖也无所谓 ——
		// 每个扇区只记一次，所以顺序无关）。
		static bool RestoreFrom(
			SectorIo &                    io,
			const std::filesystem::path & file,
			size_t &                      chunksRestored,
			std::wstring &                error);

		size_t Bytes() const;

		size_t Count() const;

	private:
		std::map<uint64_t, std::vector<uint8_t>> sectors_; // 扇区偏移 -> 512 字节原内容
	};

	// 把镜像文件当扇区设备（自检用）
	class FileSectorIo : public SectorIo
	{
	public:
		bool Open(const std::filesystem::path &path, bool create, uint64_t sizeBytes, std::wstring &error);

		bool Read(uint64_t offset, void *dst, size_t len) override;

		bool Write(uint64_t offset, const void *src, size_t len) override;

		void Close();

		std::wstring failure() const override
		{
			return failure_;
		}

		~FileSectorIo() override;

	private:
		HANDLE       handle_ = INVALID_HANDLE_VALUE;
		std::wstring failure_;
	};

	// 普通物理盘句柄（\\.\PhysicalDriveN）：不装任何驱动，走 Windows 自己的 disk.sys 栈。
	//
	// 与 WinDisk 驱动的区别：不需要装载未签名驱动、不需要 DSE 被解开；代价是受 Windows
	// 自己那条限制约束 —— 写入落在**已挂载文件系统**覆盖的扇区时会返回 ERROR_ACCESS_DENIED(5)，
	// 所以调用方要先对该卷 FSCTL_LOCK_VOLUME / FSCTL_DISMOUNT_VOLUME（见 LockCoveringVolume）。
	class PhysicalDriveSectorIo : public SectorIo
	{
	public:
		bool Open(uint32_t diskNumber, std::wstring &error);

		bool Read(uint64_t offset, void *dst, size_t len) override;

		bool Write(uint64_t offset, const void *src, size_t len) override;

		void Close();

		~PhysicalDriveSectorIo() override;

		bool valid() const
		{
			return handle_ != INVALID_HANDLE_VALUE;
		}

		std::wstring failure() const override
		{
			return failure_;
		}

	private:
		// 非 512 对齐的请求走"读-改-写"，与 WinDisk 的语义保持一致
		bool aligned(uint64_t offset, size_t len) const;

		// 立刻抓 GetLastError（调用点必须紧挨失败的 API）
		void note(const wchar_t *what);

		HANDLE       handle_ = INVALID_HANDLE_VALUE;
		std::wstring failure_;
	};

	// 把覆盖 [diskOffset, diskOffset+length) 的那个卷打开并锁/卸载，让裸写物理盘成为唯一
	// 写入者（返回的句柄必须在写完后 CloseHandle）。找不到这样的卷（ESP 常见：没有挂载点）
	// 时返回 true + INVALID_HANDLE_VALUE —— 那不是错误。
	//
	// 打开方式必须是**只读**：卷的 FAT 被 Windows 挂着时，带写权限的 CreateFile 直接
	// ERROR_ACCESS_DENIED(5)，于是"找不到要锁的卷" → 裸写被内核拒 → 表现为一次莫名的写盘
	// 失败（实测就是这个坑）。锁定用 FSCTL_LOCK_VOLUME，成功后 FSCTL_DISMOUNT_VOLUME：
	// 卷一旦锁定/卸载，Windows 就允许对**物理盘**上属于它的扇区做直写。
	//
	// volumePath 输出被锁的卷设备名（没找到时清空）—— 调用方把它打进日志，
	// 免得"锁没锁上"只能靠猜。
	bool LockCoveringVolume(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		HANDLE &      volumeHandle,
		std::wstring &volumePath,
		std::wstring &error);
} // namespace icemelt
