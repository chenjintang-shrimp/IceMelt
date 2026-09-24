#include "raw/fat_volume.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "ff.h"     // third_party/FatFs（上游原文）+ 我们的 ffconf.h
#include "diskio.h" // disk_status/disk_read/... 与 DSTATUS/DRESULT（ff.h 不含这些）

#include "util.h"

// FatFs 的 FATFS 对象（含 512 字节窗口）只在 .cpp 里露出来：头文件保持 FF_ 宏不越界。
struct FatFsHandle
{
	FATFS fs{};
};

namespace icemelt
{
	namespace
	{
		constexpr uint32_t kSector    = 512;       // FatFs 与磁盘之间的 I/O 单位
		constexpr size_t   kChunk     = 64 * 1024; // 一次读写的数据块
		constexpr UINT     kMaxDrives = FF_VOLUMES; // 必须与 ffconf.h 一致

		static_assert(
			kMaxDrives == 4,
			"ffconf.h 的 FF_VOLUMES 改了，这里的槽位表也得跟着改");

		// 盘号 → (设备, 分区起始偏移, 扇区数, 快照)。FatFs 的 diskio 是全局函数、按盘号取值，
		// 所以注册表只能放在文件作用域（单线程 CLI，不需要锁）。
		struct DriveCtx
		{
			SectorIo *    io          = nullptr;
			uint64_t      base        = 0;
			uint64_t      sectorCount = 0; // 只有 f_mkfs 用得到
			UndoJournal * journal     = nullptr;
			std::wstring  ioFailure; // 最近一次失败的 SectorIo 原因（含 Win32 码），诊断用
		};

		DriveCtx g_drives[kMaxDrives];

		// 自检用的 I/O 计数（单线程，直接放文件作用域）
		bool       g_countIo   = false;
		FatIoStats g_ioStats{};

		int AcquireDrive(SectorIo &io, uint64_t base, uint64_t sectorCount, UndoJournal *journal)
		{
			for (UINT i = 0; i < kMaxDrives; ++i)
			{
				if (g_drives[i].io != nullptr) continue;
				g_drives[i].io          = &io;
				g_drives[i].base        = base;
				g_drives[i].sectorCount = sectorCount;
				g_drives[i].journal     = journal;
				return static_cast<int>(i);
			}
			return -1;
		}

		void ReleaseDrive(int pdrv)
		{
			if (pdrv < 0 || pdrv >= static_cast<int>(kMaxDrives)) return;
			g_drives[pdrv] = DriveCtx{};
		}

		std::wstring DrivePath(int pdrv)
		{
			return FormatW(L"%d:", pdrv);
		}

		const wchar_t *FatFsErrorText(FRESULT res)
		{
			switch (res)
			{
			case FR_OK: return L"ok";
			case FR_DISK_ERR: return L"disk error";
			case FR_INT_ERR: return L"internal error (assertion failure)";
			case FR_NOT_READY: return L"the device is not ready";
			case FR_NO_FILE: return L"no such file";
			case FR_NO_PATH: return L"no such path";
			case FR_INVALID_NAME: return L"invalid name";
			case FR_DENIED: return L"denied (read-only / attribute / full)";
			case FR_EXIST: return L"already exists";
			case FR_INVALID_OBJECT: return L"invalid object";
			case FR_WRITE_PROTECTED: return L"write protected";
			case FR_INVALID_DRIVE: return L"invalid drive number";
			case FR_NOT_ENABLED: return L"the volume is not mounted";
			case FR_NO_FILESYSTEM: return L"no FAT filesystem";
			case FR_MKFS_ABORTED: return L"f_mkfs aborted";
			case FR_TIMEOUT: return L"timeout";
			case FR_LOCKED: return L"file is locked";
			case FR_NOT_ENOUGH_CORE: return L"out of memory";
			case FR_TOO_MANY_OPEN_FILES: return L"too many open files";
			case FR_INVALID_PARAMETER: return L"invalid parameter";
			default: return L"unknown FatFs error";
			}
		}

		// 读一个字节/字（只用来看 BPB 里的扇区大小）
		uint16_t RdU16(const uint8_t *p)
		{
			return static_cast<uint16_t>(p[0] | (p[1] << 8));
		}

		// FatFs 错误码 → 可读文本。FR_DISK_ERR 会把 diskio 层记下的原因附上 ——
		// 光说 "disk error" 等于没说（裸写被内核拒绝时唯一能说清"为什么"的就是它）。
		std::wstring FatFsErrorWithIo(FRESULT res, const std::wstring &ioFailure)
		{
			std::wstring text = FatFsErrorText(res);
			if (res == FR_DISK_ERR && !ioFailure.empty()) text += L": " + ioFailure;
			return text;
		}
	} // namespace

	// diskio 回调在文件末尾（必须在 namespace icemelt 之外 —— 它们要的是 C 链接）

	// ---------------------------------------------------------------- FatFsVolume

	FatFsVolume::~FatFsVolume()
	{
		Unmount();
	}

	bool FatFsVolume::Mount(SectorIo &io, uint64_t partitionBase, std::wstring &error)
	{
		Unmount();

		// 先自己看一眼 BPB：FatFs 按固定 512 字节扇区编译（见 ffconf.h 的取舍说明），
		// 扇区尺寸不符的卷直接拒绝，而不是让 FatFs 在"逻辑扇区 ≠ 物理扇区"里换算出错。
		uint8_t boot[kSector] = {};
		if (!io.Read(partitionBase, boot, sizeof(boot)))
		{
			error = FormatW(
				L"cannot read the boot sector at offset %llu",
				static_cast<unsigned long long>(partitionBase));
			return false;
		}
		const uint32_t bytesPerSector = RdU16(boot + 11);
		if (bytesPerSector != kSector)
		{
			error = FormatW(
				L"the volume declares %u-byte sectors; only 512 is supported",
				bytesPerSector);
			return false;
		}

		const int pdrv = AcquireDrive(io, partitionBase, 0, journal_);
		if (pdrv < 0)
		{
			error = L"no free FatFs drive slot (FF_VOLUMES)";
			return false;
		}

		std::unique_ptr<FatFsHandle> handle(new FatFsHandle{});
		const FRESULT                res = f_mount(&handle->fs, DrivePath(pdrv).c_str(), 1);
		if (res != FR_OK)
		{
			ReleaseDrive(pdrv);
			error = FormatW(L"f_mount: %ls", FatFsErrorWithIo(res, lastIoFailure()).c_str());
			return false;
		}

		handle_    = handle.release();
		pdrv_      = pdrv;
		mounted_   = true;
		freeValid_ = false;
		return true;
	}

	void FatFsVolume::Unmount()
	{
		if (mounted_)
		{
			f_mount(nullptr, DrivePath(pdrv_).c_str(), 0);
			mounted_ = false;
		}
		if (handle_ != nullptr)
		{
			delete handle_;
			handle_ = nullptr;
		}
		ReleaseDrive(pdrv_);
		pdrv_ = -1;
	}

	const wchar_t *FatFsVolume::type() const
	{
		if (!mounted_ || handle_ == nullptr) return L"(not mounted)";
		switch (handle_->fs.fs_type)
		{
		case FS_FAT12: return L"FAT12";
		case FS_FAT16: return L"FAT16";
		case FS_FAT32: return L"FAT32";
		default: return L"(unknown)";
		}
	}

	uint64_t FatFsVolume::clusterSize() const
	{
		if (!mounted_ || handle_ == nullptr) return 0;
		return static_cast<uint64_t>(handle_->fs.csize) * kSector;
	}

	uint64_t FatFsVolume::volumeBytes() const
	{
		if (!mounted_ || handle_ == nullptr) return 0;
		// n_fatent = 簇数 + 2（两个保留表项）
		const uint64_t clusters = handle_->fs.n_fatent > 2 ? handle_->fs.n_fatent - 2 : 0;
		return clusters * clusterSize();
	}

	void FatFsVolume::refreshFree()
	{
		if (!mounted_) return;
		DWORD   nclst = 0;
		FATFS * fs    = nullptr;
		if (f_getfree(DrivePath(pdrv_).c_str(), &nclst, &fs) == FR_OK)
		{
			freeBytes_ = static_cast<uint64_t>(nclst) * clusterSize();
			freeValid_ = true;
		}
		else
		{
			freeValid_ = false;
		}
	}

	uint64_t FatFsVolume::freeBytes()
	{
		if (!freeValid_) refreshFree();
		return freeBytes_;
	}

	bool FatFsVolume::makePath(const std::string &pathUtf8, std::wstring &out, std::wstring &error)
	{
		if (!mounted_)
		{
			error = L"the volume is not mounted";
			return false;
		}
		std::wstring rel = Widen(pathUtf8);
		for (wchar_t &c: rel)
			if (c == L'\\') c = L'/';
		while (!rel.empty() && rel.front() == L'/') rel.erase(rel.begin());
		while (!rel.empty() && rel.back() == L'/') rel.pop_back();
		out = DrivePath(pdrv_) + L"/";
		out += rel;
		return true;
	}

	bool FatFsVolume::ensureDirectory(const std::string &dirUtf8, std::wstring &error)
	{
		std::string dir = dirUtf8;
		for (char &c: dir)
			if (c == '/') c = '\\';
		while (!dir.empty() && dir.front() == '\\') dir.erase(dir.begin());
		while (!dir.empty() && dir.back() == '\\') dir.pop_back();
		if (dir.empty()) return true; // 根目录

		std::string prefix;
		size_t      start = 0;
		while (start <= dir.size())
		{
			const size_t pos  = dir.find('\\', start);
			const size_t end  = pos == std::string::npos ? dir.size() : pos;
			const std::string component = dir.substr(start, end - start);
			start = end + 1;
			if (component.empty()) continue;
			if (!prefix.empty()) prefix += "\\";
			prefix += component;

			std::wstring wpath;
			if (!makePath(prefix, wpath, error)) return false;
			FILINFO fno{};
			const FRESULT st = f_stat(wpath.c_str(), &fno);
			if (st == FR_OK)
			{
				if ((fno.fattrib & AM_DIR) == 0)
				{
					error = FormatW(L"%hs already exists and is not a directory", prefix.c_str());
					return false;
				}
				continue;
			}
			if (st != FR_NO_FILE && st != FR_NO_PATH)
			{
				error = FormatW(L"f_stat %hs: %ls", prefix.c_str(), FatFsErrorWithIo(st, lastIoFailure()).c_str());
				return false;
			}
			const FRESULT mk = f_mkdir(wpath.c_str());
			if (mk != FR_OK && mk != FR_EXIST)
			{
				error = FormatW(L"f_mkdir %hs: %ls", prefix.c_str(), FatFsErrorWithIo(mk, lastIoFailure()).c_str());
				return false;
			}
		}
		return true;
	}

	bool FatFsVolume::MakeDirectory(const std::string &path, std::wstring &error)
	{
		return ensureDirectory(path, error);
	}

	bool FatFsVolume::Exists(const std::string &path, uint64_t *sizeOut, bool *isDir, std::wstring &error)
	{
		error.clear();
		std::wstring wpath;
		if (!makePath(path, wpath, error)) return false;
		FILINFO       fno{};
		const FRESULT res = f_stat(wpath.c_str(), &fno);
		if (res == FR_NO_FILE || res == FR_NO_PATH) return false;
		if (res != FR_OK)
		{
			error = FormatW(L"f_stat %hs: %ls", path.c_str(), FatFsErrorWithIo(res, lastIoFailure()).c_str());
			return false;
		}
		if (sizeOut != nullptr) *sizeOut = static_cast<uint64_t>(fno.fsize);
		if (isDir != nullptr) *isDir = (fno.fattrib & AM_DIR) != 0;
		return true;
	}

	bool FatFsVolume::WriteFile(
		const std::string &                                   path,
		uint64_t                                              size,
		const std::function<bool(uint64_t, void *, size_t)> & reader,
		std::wstring &                                        error)
	{
		const size_t slash = path.find_last_of('\\');
		if (slash != std::string::npos &&
			!ensureDirectory(path.substr(0, slash), error))
			return false;

		std::wstring wpath;
		if (!makePath(path, wpath, error)) return false;

		FIL           fil{};
		const FRESULT openRes = f_open(&fil, wpath.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
		if (openRes != FR_OK)
		{
			error = FormatW(L"f_open %hs (write): %ls", path.c_str(), FatFsErrorWithIo(openRes, lastIoFailure()).c_str());
			return false;
		}

		std::vector<uint8_t> buf(kChunk);
		uint64_t             offset = 0;
		while (offset < size)
		{
			const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, size - offset));
			if (!reader(offset, buf.data(), want))
			{
				f_close(&fil);
				error = FormatW(
					L"the source of %hs ended early at offset %llu",
					path.c_str(),
					static_cast<unsigned long long>(offset));
				return false;
			}
			UINT          written = 0;
			const FRESULT res     = f_write(&fil, buf.data(), static_cast<UINT>(want), &written);
			if (res != FR_OK || written != want)
			{
				f_close(&fil);
				error = FormatW(
					L"f_write %hs at offset %llu: %ls (%u of %zu bytes)",
					path.c_str(),
					static_cast<unsigned long long>(offset),
					FatFsErrorWithIo(res, lastIoFailure()).c_str(),
					written,
					want);
				return false;
			}
			offset += want;
		}

		// f_close 内含 f_sync：FAT、目录条目、FSInfo 都在这里落地
		const FRESULT closeRes = f_close(&fil);
		if (closeRes != FR_OK)
		{
			error = FormatW(L"f_close %hs: %ls", path.c_str(), FatFsErrorWithIo(closeRes, lastIoFailure()).c_str());
			return false;
		}
		freeValid_ = false;
		return true;
	}

	bool FatFsVolume::ReadFile(const std::string &path, std::vector<uint8_t> &out, std::wstring &error)
	{
		std::wstring wpath;
		if (!makePath(path, wpath, error)) return false;

		FIL           fil{};
		const FRESULT openRes = f_open(&fil, wpath.c_str(), FA_READ);
		if (openRes != FR_OK)
		{
			error = FormatW(L"f_open %hs (read): %ls", path.c_str(), FatFsErrorWithIo(openRes, lastIoFailure()).c_str());
			return false;
		}
		const uint64_t size = static_cast<uint64_t>(f_size(&fil));
		out.assign(static_cast<size_t>(size), 0);
		uint64_t offset = 0;
		while (offset < size)
		{
			const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, size - offset));
			UINT         got  = 0;
			const FRESULT res = f_read(&fil, out.data() + offset, static_cast<UINT>(want), &got);
			if (res != FR_OK || got != want)
			{
				f_close(&fil);
				error = FormatW(
					L"f_read %hs at offset %llu: %ls (%u of %zu bytes)",
					path.c_str(),
					static_cast<unsigned long long>(offset),
					FatFsErrorWithIo(res, lastIoFailure()).c_str(),
					got,
					want);
				return false;
			}
			offset += want;
		}
		const FRESULT closeRes = f_close(&fil);
		if (closeRes != FR_OK)
		{
			error = FormatW(L"f_close %hs: %ls", path.c_str(), FatFsErrorWithIo(closeRes, lastIoFailure()).c_str());
			return false;
		}
		return true;
	}

	bool FatFsVolume::VerifyFile(
		const std::string &                                   path,
		uint64_t                                              size,
		const std::function<bool(uint64_t, void *, size_t)> & reader,
		std::wstring &                                        error)
	{
		std::wstring wpath;
		if (!makePath(path, wpath, error)) return false;

		FIL           fil{};
		const FRESULT openRes = f_open(&fil, wpath.c_str(), FA_READ);
		if (openRes != FR_OK)
		{
			error = FormatW(L"f_open %hs (verify): %ls", path.c_str(), FatFsErrorWithIo(openRes, lastIoFailure()).c_str());
			return false;
		}
		const uint64_t onDisk = static_cast<uint64_t>(f_size(&fil));
		if (onDisk != size)
		{
			f_close(&fil);
			error = FormatW(
				L"%hs is %llu bytes on disk, expected %llu",
				path.c_str(),
				static_cast<unsigned long long>(onDisk),
				static_cast<unsigned long long>(size));
			return false;
		}

		std::vector<uint8_t> got(kChunk), want(kChunk);
		uint64_t             offset = 0;
		while (offset < size)
		{
			const size_t n    = static_cast<size_t>(std::min<uint64_t>(kChunk, size - offset));
			UINT         read = 0;
			const FRESULT res = f_read(&fil, got.data(), static_cast<UINT>(n), &read);
			if (res != FR_OK || read != n)
			{
				f_close(&fil);
				error = FormatW(
					L"f_read %hs at offset %llu: %ls (%u of %zu bytes)",
					path.c_str(),
					static_cast<unsigned long long>(offset),
					FatFsErrorWithIo(res, lastIoFailure()).c_str(),
					read,
					n);
				return false;
			}
			if (!reader(offset, want.data(), n))
			{
				f_close(&fil);
				error = FormatW(L"cannot re-read the source of %hs at offset %llu", path.c_str(),
				                static_cast<unsigned long long>(offset));
				return false;
			}
			if (std::memcmp(got.data(), want.data(), n) != 0)
			{
				f_close(&fil);
				error = FormatW(
					L"%hs differs from the source at offset %llu",
					path.c_str(),
					static_cast<unsigned long long>(offset));
				return false;
			}
			offset += n;
		}
		const FRESULT closeRes = f_close(&fil);
		if (closeRes != FR_OK)
		{
			error = FormatW(L"f_close %hs: %ls", path.c_str(), FatFsErrorWithIo(closeRes, lastIoFailure()).c_str());
			return false;
		}
		return true;
	}

	void FatFsVolume::SetJournal(UndoJournal *journal)
	{
		journal_ = journal;
		if (pdrv_ >= 0 && pdrv_ < static_cast<int>(kMaxDrives))
			g_drives[pdrv_].journal = journal;
	}

	std::wstring FatFsVolume::lastIoFailure() const
	{
		if (pdrv_ < 0 || pdrv_ >= static_cast<int>(kMaxDrives)) return {};
		return g_drives[pdrv_].ioFailure;
	}

	// ---------------------------------------------------------------- I/O 计数

	void EnableFatIoCounters(bool on)
	{
		g_countIo = on;
	}

	void ResetFatIoCounters()
	{
		g_ioStats = FatIoStats{};
	}

	FatIoStats FatIoCounters()
	{
		return g_ioStats;
	}

	// ---------------------------------------------------------------- 自检用格式化

	bool FormatFat32ForTest(
		SectorIo &    io,
		uint64_t      base,
		uint64_t      sizeBytes,
		uint32_t      bytesPerSector,
		uint32_t      sectorsPerCluster,
		std::wstring &error)
	{
		if (bytesPerSector != kSector)
		{
			error = FormatW(L"only 512-byte sectors are supported (%u given)", bytesPerSector);
			return false;
		}
		if (sizeBytes < 64 * kSector)
		{
			error = L"the image is too small to format";
			return false;
		}
		const int pdrv = AcquireDrive(io, base, sizeBytes / kSector, nullptr);
		if (pdrv < 0)
		{
			error = L"no free FatFs drive slot (FF_VOLUMES)";
			return false;
		}
		MKFS_PARM opt{};
		opt.fmt     = FM_FAT32 | FM_SFD; // 无分区表：镜像本身就是"分区内容"（与真实 ESP 同形）
		// 两份 FAT：Windows 格式化 ESP 就是这个形状。这条必须显式指定 —— 只建一份会让
		// "改动镜像到所有 FAT 副本"那段代码路径完全测不到，而真实 ESP 上它一定会跑。
		opt.n_fat   = 2;
		opt.au_size = static_cast<DWORD>(sectorsPerCluster) * kSector; // 簇大小（字节）；0 = 自动
		BYTE          work[FF_MAX_SS] = {};
		const FRESULT res = f_mkfs(DrivePath(pdrv).c_str(), &opt, work, sizeof(work));
		const std::wstring ioFailure = g_drives[pdrv].ioFailure; // ReleaseDrive 会清掉
		ReleaseDrive(pdrv);
		if (res != FR_OK)
		{
			error = FormatW(L"f_mkfs: %ls", FatFsErrorWithIo(res, ioFailure).c_str());
			return false;
		}
		return true;
	}
} // namespace icemelt

// ---------------------------------------------------------------- diskio 回调
//
// 这一组必须在 **namespace icemelt 之外**：ff.c 是用 C 编译器编的，它引用的符号带 C 链接
// （diskio.h 里用 extern "C" 声明），放进命名空间就会变成 C++ 修饰名 —— 实测表现为
// LNK2019 里六个 disk_*/get_fattime 全部"无法解析"。
// 盘号表/计数器在 icemelt 的匿名命名空间里，用限定名访问即可。
//
// 这层是"FatFs 与磁盘之间"的唯一通道，所以两件事都钉在这里：
//   * 每个写入前先做 undo 快照（取不到原内容就让写失败，宁可中止部署）
//   * 统计调用次数（自检用它把"每簇一次系统调用"的退化挡在门外）

DSTATUS disk_status(BYTE pdrv)
{
	if (pdrv >= icemelt::kMaxDrives || icemelt::g_drives[pdrv].io == nullptr) return STA_NOINIT;
	return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv); // 我们的设备（物理盘句柄/镜像文件）总是已就绪
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (icemelt::g_countIo)
	{
		++icemelt::g_ioStats.reads;
		icemelt::g_ioStats.readSectors += count;
	}
	if (pdrv >= icemelt::kMaxDrives || icemelt::g_drives[pdrv].io == nullptr) return RES_NOTRDY;
	const icemelt::DriveCtx &d = icemelt::g_drives[pdrv];
	if (count == 0) return RES_OK;
	const uint64_t offset = d.base + static_cast<uint64_t>(sector) * icemelt::kSector;
	if (d.io->Read(offset, buff, static_cast<size_t>(count) * icemelt::kSector)) return RES_OK;
	icemelt::g_drives[pdrv].ioFailure = d.io->failure();
	return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (icemelt::g_countIo)
	{
		++icemelt::g_ioStats.writes;
		icemelt::g_ioStats.writeSectors += count;
	}
	if (pdrv >= icemelt::kMaxDrives || icemelt::g_drives[pdrv].io == nullptr) return RES_NOTRDY;
	const icemelt::DriveCtx &d = icemelt::g_drives[pdrv];
	if (count == 0) return RES_OK;
	const uint64_t offset = d.base + static_cast<uint64_t>(sector) * icemelt::kSector;
	const size_t   len    = static_cast<size_t>(count) * icemelt::kSector;
	// 快照必须在**写之前**做；取不到原内容就宁可让这次写失败（调用方会中止部署）
	if (d.journal != nullptr)
	{
		std::wstring snapshotError;
		if (!d.journal->Capture(*d.io, offset, len, snapshotError))
		{
			icemelt::g_drives[pdrv].ioFailure =
					L"undo snapshot: " + (snapshotError.empty() ? d.io->failure() : snapshotError);
			return RES_ERROR;
		}
	}
	if (d.io->Write(offset, buff, len)) return RES_OK;
	icemelt::g_drives[pdrv].ioFailure = d.io->failure();
	return RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (pdrv >= icemelt::kMaxDrives || icemelt::g_drives[pdrv].io == nullptr) return RES_NOTRDY;
	const icemelt::DriveCtx &d = icemelt::g_drives[pdrv];
	switch (cmd)
	{
	case CTRL_SYNC: return RES_OK; // 句柄是 write-through / 文件句柄本来就是同步写
	case GET_SECTOR_SIZE:
		*static_cast<WORD *>(buff) = icemelt::kSector;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*static_cast<DWORD *>(buff) = 1; // 擦除块：我们不做 TRIM
		return RES_OK;
	case GET_SECTOR_COUNT:
		*static_cast<LBA_t *>(buff) = static_cast<LBA_t>(d.sectorCount);
		return RES_OK;
	default: return RES_PARERR;
	}
}

DWORD get_fattime(void)
{
	SYSTEMTIME st{};
	::GetLocalTime(&st);
	const unsigned year = st.wYear < 1980 ? 1980 : st.wYear;
	return (static_cast<DWORD>(year - 1980) << 25) |
			(static_cast<DWORD>(st.wMonth) << 21) |
			(static_cast<DWORD>(st.wDay) << 16) |
			(static_cast<DWORD>(st.wHour) << 11) |
			(static_cast<DWORD>(st.wMinute) << 5) |
			(static_cast<DWORD>(st.wSecond / 2));
}
