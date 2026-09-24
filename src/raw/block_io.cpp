#include "raw/block_io.h"

#include <winioctl.h> // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS / FSCTL_LOCK_VOLUME

#include <cstring>

#include "util.h"

namespace icemelt
{
	namespace
	{
		// 隐藏分区的卷设备不在 Win32 卷列表里（FindFirstVolumeW 枚举不到），但它有
		// NT 设备对象：\\?\GLOBALROOT\Device\HarddiskVolumeN。用它按 extents 找出
		// "覆盖目标范围的卷"——这正是裸写被拒(5)时唯一没被查过的地方。
		//
		// 命中时返回句柄（GENERIC_READ 打开，FSCTL_LOCK_VOLUME 只需要读句柄）；
		// 没有任何 HarddiskVolumeN 覆盖目标范围时返回 INVALID_HANDLE_VALUE。
		HANDLE OpenCoveringHarddiskVolume(
			uint32_t      diskNumber,
			uint64_t      diskOffset,
			uint64_t      length,
			std::wstring &openedPath,
			std::wstring &note)
		{
			for (uint32_t n = 0; n < 64; ++n)
			{
				const std::wstring path =
						FormatW(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume%u", n);
				HANDLE h = ::CreateFileW(
					path.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE,
					nullptr,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					nullptr);
				if (h == INVALID_HANDLE_VALUE)
				{
					const DWORD err = ::GetLastError();
					// 设备不存在 = 编号扫过头了；err=5（能开却拒绝）值得记下来 ——
					// 它可能正是覆盖 ESP 的那个卷（过滤器挡了打开）。
					if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND &&
						err != ERROR_NO_SUCH_DEVICE && err != ERROR_BAD_UNIT)
						note += FormatW(L"HarddiskVolume%u: open %lu; ", n, err);
					continue;
				}
				std::vector<uint8_t> buf(1024);
				DWORD                bytes = 0;
				const BOOL ok = ::DeviceIoControl(
					h,
					IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
					nullptr,
					0,
					buf.data(),
					static_cast<DWORD>(buf.size()),
					&bytes,
					nullptr);
				bool covers = false;
				if (ok && bytes >= sizeof(DWORD))
				{
					const auto *extents =
							reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf.data());
					for (DWORD i = 0; i < extents->NumberOfDiskExtents && !covers; ++i)
					{
						const auto &e = extents->Extents[i];
						if (e.DiskNumber != diskNumber) continue;
						const uint64_t start = e.StartingOffset.QuadPart;
						const uint64_t len   = e.ExtentLength.QuadPart;
						if (diskOffset >= start && diskOffset + length <= start + len)
							covers = true;
					}
				}
				if (covers)
				{
					openedPath = path;
					return h;
				}
				::CloseHandle(h);
			}
			return INVALID_HANDLE_VALUE;
		}
	} // namespace

	namespace
	{
		constexpr uint32_t kSector = 512; // 本实现只接受 512 字节扇区（ESP 恒为 512）

		// 抓当前线程的 Win32 错误并格式化成可读文本。
		// 调用点必须**紧挨**失败的 API —— GetLastError 会被任何一次后续调用覆盖。
		std::wstring Win32Failure(const wchar_t *what)
		{
			const DWORD err = ::GetLastError();
			return FormatW(L"%ls failed: %lu%s", what, err,
			               err == ERROR_ACCESS_DENIED
					               ? L" (access denied: the covering volume is mounted and not locked?)"
					               : L"");
		}

		uint32_t RdU32(const uint8_t *p)
		{
			return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
			                             (static_cast<uint32_t>(p[3]) << 24));
		}

		void WrU32(uint8_t *p, uint32_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
			p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
			p[3] = static_cast<uint8_t>(v >> 24);
		}
	} // namespace

	// ---------------------------------------------------------------- UndoJournal

	bool UndoJournal::Capture(SectorIo &io, uint64_t offset, size_t len, std::wstring &error)
	{
		if (len == 0) return true;
		const uint64_t first = (offset / kSector) * kSector;
		const uint64_t last  = (offset + len - 1) / kSector * kSector;
		for (uint64_t sector = first; sector <= last; sector += kSector)
		{
			if (sectors_.count(sector) != 0) continue;
			std::vector<uint8_t> bytes(kSector);
			if (!io.Read(sector, bytes.data(), kSector))
			{
				error = FormatW(
					L"undo snapshot: cannot read sector at %llu",
					static_cast<unsigned long long>(sector));
				return false;
			}
			sectors_.emplace(sector, std::move(bytes));
		}
		return true;
	}

	size_t UndoJournal::Bytes() const
	{
		return sectors_.size() * kSector;
	}

	size_t UndoJournal::Count() const
	{
		return sectors_.size();
	}

	bool UndoJournal::SaveTo(const std::filesystem::path &file, uint32_t diskNumber, std::wstring &error) const
	{
		HANDLE h = ::CreateFileW(
			file.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = FormatW(L"cannot create %ls: %lu", file.c_str(), ::GetLastError());
			return false;
		}
		std::vector<uint8_t> blob;
		const uint8_t magic[16] = {'I', 'C', 'E', 'M', 'E', 'L', 'T', 'U', 'N', 'D', 'O', '1', 0, 0, 0, 0};
		blob.insert(blob.end(), magic, magic + sizeof(magic));
		uint8_t count[8];
		WrU32(count, diskNumber);
		WrU32(count + 4, static_cast<uint32_t>(sectors_.size()));
		blob.insert(blob.end(), count, count + 8);
		for (const auto &kv: sectors_)
		{
			uint8_t head[16];
			WrU32(head, static_cast<uint32_t>(kv.first & 0xFFFFFFFFu));
			WrU32(head + 4, static_cast<uint32_t>(kv.first >> 32));
			WrU32(head + 8, kSector);
			WrU32(head + 12, 0);
			blob.insert(blob.end(), head, head + 16);
			blob.insert(blob.end(), kv.second.begin(), kv.second.end());
		}
		DWORD written = 0;
		const BOOL ok = ::WriteFile(
			h,
			blob.data(),
			static_cast<DWORD>(blob.size()),
			&written,
			nullptr);
		::CloseHandle(h);
		if (!ok || written != blob.size())
		{
			error = FormatW(L"cannot write %ls: %lu", file.c_str(), ::GetLastError());
			return false;
		}
		return true;
	}

	bool UndoJournal::ReadDiskNumber(
		const std::filesystem::path &file,
		uint32_t &                    diskNumber,
		std::wstring &                error)
	{
		HANDLE h = ::CreateFileW(
			file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = FormatW(L"cannot open %ls: %lu", file.c_str(), ::GetLastError());
			return false;
		}
		uint8_t head[24] = {};
		DWORD   read     = 0;
		const BOOL ok = ::ReadFile(h, head, sizeof(head), &read, nullptr);
		::CloseHandle(h);
		if (!ok || read < 24 || std::memcmp(head, "ICEMELTUNDO1", 12) != 0)
		{
			error = L"undo file has an unknown format";
			return false;
		}
		diskNumber = RdU32(head + 16);
		return true;
	}

	bool UndoJournal::RestoreFrom(
		SectorIo &                    io,
		const std::filesystem::path & file,
		size_t &                      chunksRestored,
		std::wstring &                error)
	{
		HANDLE h = ::CreateFileW(
			file.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = FormatW(L"cannot open %ls: %lu", file.c_str(), ::GetLastError());
			return false;
		}
		LARGE_INTEGER size{};
		::GetFileSizeEx(h, &size);

		std::vector<uint8_t> blob(static_cast<size_t>(size.QuadPart));
		DWORD                read = 0;
		const BOOL           ok =
				::ReadFile(h, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr);
		::CloseHandle(h);
		if (!ok || read != blob.size() || blob.size() < 24)
		{
			error = L"undo file is truncated or unreadable";
			return false;
		}
		if (std::memcmp(blob.data(), "ICEMELTUNDO1", 12) != 0)
		{
			error = L"undo file has an unknown format";
			return false;
		}
		const uint32_t count = RdU32(blob.data() + 20);
		size_t         pos   = 24;
		chunksRestored       = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (pos + 16 > blob.size())
			{
				error = L"undo file is truncated (record header)";
				return false;
			}
			const uint64_t offset =
					static_cast<uint64_t>(RdU32(blob.data() + pos)) |
					(static_cast<uint64_t>(RdU32(blob.data() + pos + 4)) << 32);
			const uint32_t len = RdU32(blob.data() + pos + 8);
			pos += 16;
			if (pos + len > blob.size())
			{
				error = L"undo file is truncated (record body)";
				return false;
			}
			if (!io.Write(offset, blob.data() + pos, len))
			{
				error = FormatW(
					L"undo: cannot write back the sector at %llu",
					static_cast<unsigned long long>(offset));
				return false;
			}
			pos += len;
			++chunksRestored;
		}
		return true;
	}

	// ---------------------------------------------------------------- FileSectorIo

	FileSectorIo::~FileSectorIo()
	{
		Close();
	}

	bool FileSectorIo::Open(const std::filesystem::path &path, bool create, uint64_t sizeBytes, std::wstring &error)
	{
		Close();
		handle_ = ::CreateFileW(
			path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			create ? CREATE_ALWAYS : OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (handle_ == INVALID_HANDLE_VALUE)
		{
			error = FormatW(L"cannot open %ls: %lu", path.c_str(), ::GetLastError());
			return false;
		}
		if (create)
		{
			LARGE_INTEGER li{};
			li.QuadPart = static_cast<LONGLONG>(sizeBytes);
			if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN) ||
				!::SetEndOfFile(handle_))
			{
				error = FormatW(L"cannot size %ls", path.c_str());
				Close();
				return false;
			}
		}
		return true;
	}

	void FileSectorIo::Close()
	{
		if (handle_ != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(handle_);
			handle_ = INVALID_HANDLE_VALUE;
		}
	}

	bool FileSectorIo::Read(uint64_t offset, void *dst, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE)
		{
			failure_ = L"the image file is not open";
			return false;
		}
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(offset);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
		{
			failure_ = Win32Failure(L"SetFilePointerEx");
			return false;
		}
		DWORD read = 0;
		if (!::ReadFile(handle_, dst, static_cast<DWORD>(len), &read, nullptr) || read != len)
		{
			failure_ = Win32Failure(L"ReadFile");
			return false;
		}
		return true;
	}

	bool FileSectorIo::Write(uint64_t offset, const void *src, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE)
		{
			failure_ = L"the image file is not open";
			return false;
		}
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(offset);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
		{
			failure_ = Win32Failure(L"SetFilePointerEx");
			return false;
		}
		DWORD written = 0;
		if (!::WriteFile(handle_, src, static_cast<DWORD>(len), &written, nullptr) || written != len)
		{
			failure_ = Win32Failure(L"WriteFile");
			return false;
		}
		return true;
	}

	// ------------------------------------------------------ PhysicalDriveSectorIo

	PhysicalDriveSectorIo::~PhysicalDriveSectorIo()
	{
		Close();
	}

	bool PhysicalDriveSectorIo::Open(uint32_t diskNumber, std::wstring &error)
	{
		Close();
		const std::wstring path = FormatW(L"\\\\.\\PhysicalDrive%u", diskNumber);
		handle_ = ::CreateFileW(
			path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			// WRITE_THROUGH：写完就下发到设备，别停在缓存里 —— 裸写没有文件系统替我们刷。
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (handle_ == INVALID_HANDLE_VALUE)
		{
			const DWORD err = ::GetLastError();
			error = FormatW(
				L"cannot open %ls: %lu%s",
				path.c_str(),
				err,
				err == ERROR_ACCESS_DENIED ? L" (need an elevated token)" : L"");
			return false;
		}
		return true;
	}

	void PhysicalDriveSectorIo::Close()
	{
		if (handle_ != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(handle_);
			handle_ = INVALID_HANDLE_VALUE;
		}
	}

	bool PhysicalDriveSectorIo::aligned(uint64_t offset, size_t len) const
	{
		return (offset % kSector) == 0 && (len % kSector) == 0;
	}

	void PhysicalDriveSectorIo::note(const wchar_t *what)
	{
		failure_ = Win32Failure(what);
	}

	bool PhysicalDriveSectorIo::Read(uint64_t offset, void *dst, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE || len == 0)
		{
			failure_ = L"the physical drive is not open";
			return false;
		}
		if (aligned(offset, len))
		{
			LARGE_INTEGER li{};
			li.QuadPart = static_cast<LONGLONG>(offset);
			if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
			{
				note(L"SetFilePointerEx");
				return false;
			}
			DWORD read = 0;
			if (!::ReadFile(handle_, dst, static_cast<DWORD>(len), &read, nullptr) || read != len)
			{
				note(L"ReadFile");
				return false;
			}
			return true;
		}
		// 非对齐：读-改-写覆盖到的整扇区
		const uint64_t first = (offset / kSector) * kSector;
		const uint64_t last  = (offset + len - 1) / kSector * kSector;
		const size_t   total = static_cast<size_t>(last - first + kSector);
		std::vector<uint8_t> buf(total);
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
		{
			note(L"SetFilePointerEx");
			return false;
		}
		DWORD read = 0;
		if (!::ReadFile(handle_, buf.data(), static_cast<DWORD>(total), &read, nullptr) ||
			read != total)
		{
			note(L"ReadFile");
			return false;
		}
		std::memcpy(dst, buf.data() + (offset - first), len);
		return true;
	}

	bool PhysicalDriveSectorIo::Write(uint64_t offset, const void *src, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE || len == 0)
		{
			failure_ = L"the physical drive is not open";
			return false;
		}
		if (aligned(offset, len))
		{
			LARGE_INTEGER li{};
			li.QuadPart = static_cast<LONGLONG>(offset);
			if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
			{
				note(L"SetFilePointerEx");
				return false;
			}
			DWORD written = 0;
			if (!::WriteFile(handle_, src, static_cast<DWORD>(len), &written, nullptr) ||
				written != len)
			{
				note(L"WriteFile");
				return false;
			}
			return true;
		}
		const uint64_t first = (offset / kSector) * kSector;
		const uint64_t last  = (offset + len - 1) / kSector * kSector;
		const size_t   total = static_cast<size_t>(last - first + kSector);
		std::vector<uint8_t> buf(total);
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
		{
			note(L"SetFilePointerEx");
			return false;
		}
		DWORD read = 0;
		if (!::ReadFile(handle_, buf.data(), static_cast<DWORD>(total), &read, nullptr) ||
			read != total)
		{
			note(L"ReadFile (read-modify-write)");
			return false;
		}
		std::memcpy(buf.data() + (offset - first), src, len);
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
		{
			note(L"SetFilePointerEx");
			return false;
		}
		DWORD written = 0;
		if (!::WriteFile(handle_, buf.data(), static_cast<DWORD>(total), &written, nullptr) ||
			written != total)
		{
			note(L"WriteFile (read-modify-write)");
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------ 卷锁/卸载

	bool LockCoveringVolume(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		HANDLE &      volumeHandle,
		std::wstring &volumePath,
		std::wstring &note,
		std::wstring &error)
	{
		volumeHandle = INVALID_HANDLE_VALUE;
		volumePath.clear();
		note.clear();
		uint32_t seen = 0, unopenable = 0;
		DWORD    firstOpenErr = 0;
		wchar_t name[MAX_PATH] = {};
		HANDLE  find = ::FindFirstVolumeW(name, MAX_PATH);
		if (find == INVALID_HANDLE_VALUE)
		{
			error = FormatW(L"FindFirstVolumeW failed: %lu", ::GetLastError());
			return false;
		}
		bool found = false;
		do
		{
			std::wstring path(name);
			if (!path.empty() && path.back() == L'\\') path.pop_back();
			// **只读**打开：卷的文件系统被 Windows 挂着时，带 GENERIC_WRITE 的 CreateFile
			// 必然 ERROR_ACCESS_DENIED —— 那样就永远找不到"要锁的卷"，接下来裸写被内核
			// 拒绝，表现为一次没有任何线索的写盘失败（实测踩过）。锁/卸载只需要读句柄。
			HANDLE h = ::CreateFileW(
				path.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			++seen;
			if (h == INVALID_HANDLE_VALUE)
			{
				// 打不开的卷**不能静默跳过**：err=5 的那一个完全可能正是覆盖 ESP 的卷
				// （过滤器拒绝打开、或它没有挂载点）。把它连同错误码记进诊断，
				// 否则就会出现"日志说没有卷可锁 → 写盘被拒 → 一句莫名的失败"这种假阴性。
				const DWORD openErr = ::GetLastError();
				++unopenable;
				if (firstOpenErr == 0) firstOpenErr = openErr;
				if (unopenable <= 3) // 前几个就够定位了，别刷屏
					note += FormatW(
						L"cannot open %ls: %lu; ",
						path.c_str(),
						openErr);
				continue;
			}

			std::vector<uint8_t> buf(1024);
			DWORD                bytes = 0;
			const BOOL ok = ::DeviceIoControl(
				h,
				IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
				nullptr,
				0,
				buf.data(),
				static_cast<DWORD>(buf.size()),
				&bytes,
				nullptr);
			if (ok && bytes >= sizeof(DWORD))
			{
				const auto *extents =
						reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf.data());
				for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i)
				{
					const auto &e = extents->Extents[i];
					if (e.DiskNumber != diskNumber) continue;
					const uint64_t start = e.StartingOffset.QuadPart;
					const uint64_t len   = e.ExtentLength.QuadPart;
					if (diskOffset >= start && diskOffset + length <= start + len)
					{
						volumeHandle = h;
						found        = true;
					}
				}
			}
			if (!found) ::CloseHandle(h);
		} while (!found && ::FindNextVolumeW(find, name, MAX_PATH));
		::FindVolumeClose(find);

		if (!found)
		{
			// Win32 卷列表查不到 ≠ 没有卷：隐藏分区（GPT 属性里不带 GPT_BASIC_DATA_ATTRIBUTE_
			// NO_DRIVE_LETER / read-only 之类）的卷设备不在 FindFirstVolumeW 的结果里，但它
			// 有 NT 设备对象。这正好是我们上一轮踩的坑：日志说"没有要锁的卷"，随后第一次
			// 写盘就被内核以 5 拒掉 —— 覆盖目标范围的卷明明存在、挂着 FAT、却没被锁。
			// GLOBALROOT 按 extents 逐个探 HarddiskVolumeN，把漏掉的那个卷找出来。
			HANDLE gv = OpenCoveringHarddiskVolume(
					diskNumber, diskOffset, length, volumePath, note);
			if (gv != INVALID_HANDLE_VALUE)
			{
				volumeHandle = gv;
				found        = true;
			}
		}
		if (!found)
		{
			// 确实没有任何卷覆盖目标范围：那就没什么要锁的，裸写本来就允许。
			// 枚举摘要追加在逐卷诊断后面 —— "7 个卷里 5 个打不开(5)"指向过滤器/安全
			// 描述符挡了枚举，"一个卷都没有"才是真空，两者的处理完全不同。
			note += FormatW(
				L"| volume enumeration: %u listed, %u could not be opened (first error %lu)",
				seen,
				unopenable,
				firstOpenErr);
			volumeHandle = INVALID_HANDLE_VALUE;
			return true;
		}
		if (volumePath.rfind(L"\\\\?\\Volume{", 0) == 0 && !volumePath.empty() &&
			volumePath.back() == L'\\')
			volumePath.pop_back();
		note = FormatW(L"volume enumeration: %u listed, %u could not be opened", seen, unopenable);
		// 锁定：卷上有打开的文件时 FSCTL_LOCK_VOLUME 返回 ERROR_ACCESS_DENIED /
		// ERROR_SHARING_VIOLATION，等一会儿再试（杀毒/索引服务常在一瞬间持有句柄）。
		DWORD bytes  = 0;
		DWORD lockErr = ERROR_SUCCESS;
		bool  locked  = false;
		for (int attempt = 0; attempt < 20 && !locked; ++attempt)
		{
			if (::DeviceIoControl(
					volumeHandle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr))
			{
				locked = true;
				break;
			}
			lockErr = ::GetLastError();
			if (lockErr != ERROR_ACCESS_DENIED && lockErr != ERROR_SHARING_VIOLATION) break;
			::Sleep(100);
		}

		if (locked)
		{
			// 卸载：文件系统一旦离开，Windows 就允许对物理盘上属于这个卷的扇区直写。
			// 卸载失败不算错 —— 锁本身已经够了（本工具写的是物理盘句柄，不是卷句柄）。
			::DeviceIoControl(
				volumeHandle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
			return true;
		}

		// 锁不上就没有必要往下走：卷还在被文件系统管着时，裸写必然被拒（ERROR_ACCESS_DENIED），
		// 与其让调用方收到一句莫名的写盘失败，不如在这里说清原因。
		if (lockErr == ERROR_INVALID_FUNCTION || lockErr == ERROR_NOT_SUPPORTED)
		{
			// 没有文件系统在管这个卷：没有可锁的东西，裸写本来就允许
			return true;
		}
		error = FormatW(
			L"cannot lock %ls: %lu (close anything holding the volume — Explorer window, "
			L"antivirus scan — and retry)",
			volumePath.c_str(),
			lockErr);
		::CloseHandle(volumeHandle);
		volumeHandle = INVALID_HANDLE_VALUE;
		volumePath.clear();
		return false;
	}
} // namespace icemelt
