// IceMelt —— 极简 FAT16/FAT32 写入器（只做本工具需要的那几件事）
//
// 为什么需要它：ESP 是 FAT，而绕开卷过滤器的唯一手段是"经 WinDisk 直接读写物理盘扇区"
// （DeepFrz 挂在卷层、没有 SRB 能力，端口层裸写在它视野之外；唯一被 sector 影子覆盖的是
// MBR/GPT 那几个扇区 —— 见 README 的拦截层级表）。裸扇区写没有文件系统概念，于是这里实现
// 最小可用的 FAT 写入器：
//
//   * 挂在抽象的 SectorIo 上：生产用 WinDiskDevice（绕过滤器），自检用临时镜像文件
//   * FAT16 / FAT32；FAT12 直接拒绝（ESP 上不会出现；真出现了也不值得冒险写）
//   * 只支持 8.3 短名（部署要用的文件名全部落在 8.3 规格内），不写 LFN
//   * 能力：解析几何、遍历目录、建目录、写/替换文件（流式，不把 40 MB payload 全读进内存）、
//     查空闲空间、按同一视图读回校验
//   * 写前把**每个将被修改的元数据扇区**快照进 UndoJournal（可选，可回放）；
//     数据扇区不快照，但分配前会断言该簇的 FAT 表项为 0（只动空闲簇，不会踩别人的数据）
//
// 有意不做的：格式化、删除文件、修改卷标、LFN、碎片整理 —— 都不需要。

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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

	// 目录查找的键：能表达成 8.3 就比 11 字节短名字段；表达不了的（"Microsoft" 这类）
	// 走 LFN 条目组装比对 —— 真实 ESP 上的 \EFI\Microsoft 就是 LFN。
	struct FatLookupKey
	{
		bool         useShort = false;
		std::string  short11;  // 11 字节短名（空格填充）
		std::wstring longName; // LFN 名字，比对不区分大小写
	};

	class FatFileSystem
	{
	public:
		FatFileSystem() = default;

		// 解析分区起始处的 BPB 并做几何自检（扇区大小/簇大小/簇数/FAT 大小互相一致）。
		bool Mount(SectorIo &io, uint64_t partitionBase, std::wstring &error);

		bool mounted() const
		{
			return mounted_;
		}

		// "FAT16" / "FAT32"
		const wchar_t *type() const;

		uint64_t volumeBytes() const;

		uint64_t freeBytes() const; // 由 FAT 现算（ESP 很小，扫一遍很便宜）

		uint64_t clusterSize() const;

		// 路径用 UTF-8，分隔符 '\'。
		// **查找**支持两种名字：8.3 短名直接比 11 字节字段；非 8.3（例如 ESP 上的
		// "Microsoft"）走 LFN 条目组装比对 —— 真实 ESP 的 \EFI\Microsoft 就是 LFN。
		// **新建**只支持 8.3（本工具新建的名字都落在 8.3 内：EFI / IceMelt / BOOTX64.EFI /
		// BOOTMGFW.EFI / STARTUP.NSH / NTFS-3G.EFI / SYSTEM / BOOTMGW.OLD）。
		bool Exists(const std::string &path, uint64_t *sizeOut, bool *isDir, std::wstring &error);

		bool MakeDirectory(const std::string &path, std::wstring &error);

		// 流式写入：size 字节由 reader(offset, dst, len) 提供。存在则替换（先写新链、再改目录
		// 条目、最后释放旧链 —— 中途崩溃也是"文件有效 + 旧链泄漏"这种可接受状态）。
		bool WriteFile(
			const std::string &                                   path,
			uint64_t                                              size,
			const std::function<bool(uint64_t, void *, size_t)> & reader,
			std::wstring &                                        error);

		// 读回（自检与写后校验用；大文件走 VerifyFile 的流式比对，避免双份内存）
		bool ReadFile(const std::string &path, std::vector<uint8_t> &out, std::wstring &error);

		bool VerifyFile(
			const std::string &                                   shortPath,
			uint64_t                                              size,
			const std::function<bool(uint64_t, void *, size_t)> & reader,
			std::wstring &                                        error);

		// 非空时，所有元数据写入前先快照（数据簇不快照，见文件头说明）
		void SetJournal(UndoJournal *journal);

		// 分配簇前逐簇断言：FAT 表项必须为 0（只动空闲簇）
		bool setStrictAllocation(bool strict)
		{
			const bool old = strictAlloc_;
			strictAlloc_ = strict;
			return old;
		}

	private:
		struct Bpb
		{
			uint32_t bytesPerSector    = 0;
			uint32_t sectorsPerCluster = 0;
			uint32_t reservedSectors   = 0;
			uint32_t numFats           = 0;
			uint32_t rootEntries       = 0;
			uint32_t fatSizeSectors    = 0;
			uint32_t totalSectors      = 0;
			uint32_t rootCluster       = 0;
			uint32_t fsinfoSector      = 0;
			uint16_t extFlags          = 0;
			uint32_t fatStartSector    = 0;
			uint32_t rootDirSector     = 0;
			uint32_t rootDirSectors    = 0;
			uint32_t dataStartSector   = 0;
			uint32_t clusterCount      = 0;
			uint32_t fatBits           = 0; // 16 / 32
		};

		struct DirEntry
		{
			uint32_t offset   = 0; // 条目在分区内的字节偏移
			uint32_t firstCluster = 0;
			uint32_t size         = 0;
			uint8_t  attr         = 0;
		};

		uint64_t clusterBytes() const;

		uint64_t clusterOffset(uint32_t cluster) const; // 数据区字节偏移（分区内相对）

		bool     readFat(uint32_t cluster, uint32_t &value, std::wstring &error);

		bool     writeFat(uint32_t cluster, uint32_t value, std::wstring &error);

		// 找 n 个空闲簇并串成链；返回首簇（0 = 失败）
		bool     allocateChain(uint32_t count, uint32_t &first, std::wstring &error);

		bool     freeChain(uint32_t first, std::wstring &error);

		// 目录遍历：resolveDir 允许顺带创建缺失的中间目录
		bool     resolveDir(const std::string &dirPath, bool create, DirEntry &out, std::wstring &error);

		bool     findInDir(
			const DirEntry &      dir,
			const FatLookupKey &  key,
			DirEntry &            out,
			bool &                found,
			uint32_t *            freeSlotOffset,
			std::wstring &        error);

		// 目录簇里的所有条目偏移（FAT16 根目录是固定区）
		bool     forEachDirSlot(
			const DirEntry &                      dir,
			const std::function<bool(uint32_t offset, const uint8_t *entry)> & fn,
			std::wstring &                        error);

		bool     writeSector(uint64_t offset, const void *data, size_t len, std::wstring &error);

		// 连续 slots 个空闲目录槽的起始偏移（必要时给目录链扩一个簇）
		bool     findFreeRun(const DirEntry &dir, uint32_t slots, uint32_t &offset, std::wstring &error);

		// 用 LFN 条目 + 自动生成的短名新建一个条目（名字表达不成 8.3 时走这里）。
		// 真实 ESP 上的 \EFI\Microsoft 就是这种结构；本工具新建的目录/文件都够短，
		// 但自检要造出与真实 ESP 同形的镜像，所以这条路实现出来。
		bool     writeLongEntry(
			const DirEntry &    dir,
			const std::string & longName,
			uint8_t             attr,
			uint32_t            firstCluster,
			uint64_t            size,
			std::wstring &      error);

		bool     flushFsInfo(std::wstring &error);

		SectorIo *       io_        = nullptr;
		uint64_t         base_      = 0;
		bool             mounted_   = false;
		bool             strictAlloc_ = true;
		Bpb              bpb_;
		UndoJournal *    journal_ = nullptr;
	};

	// 自检用的最小 FAT32 格式化：把 io[base, base+size) 造成一个空卷（只够自检用，
	// 不追求与 mkfs 的字节级一致 —— 它存在的意义是让 FAT 写入器能在**任何机器上、
	// 不碰真实文件系统**的前提下被验证）。
	bool FormatFat32ForTest(
		SectorIo &    io,
		uint64_t      base,
		uint64_t      sizeBytes,
		uint32_t      bytesPerSector,
		uint32_t      sectorsPerCluster,
		std::wstring &error);

	// 把镜像文件当扇区设备（自检用）
	class FileSectorIo : public SectorIo
	{
	public:
		bool Open(const std::filesystem::path &path, bool create, uint64_t sizeBytes, std::wstring &error);

		bool Read(uint64_t offset, void *dst, size_t len) override;

		bool Write(uint64_t offset, const void *src, size_t len) override;

		void Close();

		~FileSectorIo() override;

	private:
		HANDLE handle_ = INVALID_HANDLE_VALUE;
	};

	// 普通物理盘句柄（\\.\PhysicalDriveN）：不装任何驱动，走 Windows 自己的 disk.sys 栈。
	//
	// 为什么它可能绕得开还原软件：DfDiskLo 的 sector 账本只影子 MBR/GPT（数据区经 hook
	// stub 原样直通），DeepFrz 的 COW 挂在**卷层** —— 裸写物理盘的扇区不经过卷栈，所以
	// DeepFrz 看不到（它没有 SRB 能力）；会被 DfDiskLo 吞掉的只有分区表那几个扇区，而本
	// 工具不碰它们。
	//
	// 与 WinDisk 驱动的区别：不需要装载未签名驱动、不需要 DSE 被解开；代价是受 Windows
	// 自己那条限制约束 —— 写入落在**已挂载文件系统**覆盖的扇区时会返回 ERROR_ACCESS_DENIED(5)，
	// 所以调用方要先对该卷 FSCTL_LOCK_VOLUME / FSCTL_DISMOUNT_VOLUME（见 lockEspVolume）。
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

	private:
		// 非 512 对齐的请求走"读-改-写"，与 WinDisk 的语义保持一致
		bool aligned(uint64_t offset, size_t len) const;

		HANDLE handle_ = INVALID_HANDLE_VALUE;
	};

	// 把覆盖 [diskOffset, diskOffset+length) 的那个卷打开并锁/卸载，让裸写物理盘成为唯一
	// 写入者（返回的句柄必须在写完后 CloseHandle）。找不到这样的卷（ESP 常见：没有挂载点）
	// 时返回 true + INVALID_HANDLE_VALUE —— 那不是错误。
	bool LockCoveringVolume(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		HANDLE &      volumeHandle,
		std::wstring &error);
} // namespace icemelt
