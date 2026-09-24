#include "raw/fat.h"

#include <winioctl.h> // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS / FSCTL_LOCK_VOLUME

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string.h>

#include "util.h"

namespace icemelt
{
	namespace
	{
		constexpr uint32_t kSector = 512; // 本实现只接受 512 字节扇区（ESP 恒为 512）

		uint16_t RdU16(const uint8_t *p)
		{
			return static_cast<uint16_t>(p[0] | (p[1] << 8));
		}

		uint32_t RdU32(const uint8_t *p)
		{
			return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
			                             (static_cast<uint32_t>(p[3]) << 24));
		}

		void WrU16(uint8_t *p, uint16_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>(v >> 8);
		}

		void WrU32(uint8_t *p, uint32_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
			p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
			p[3] = static_cast<uint8_t>(v >> 24);
		}

		// 8.3 名 → 11 字节目录字段（"NAME    EXT"）。小写会被规范化成大写。
		bool MakeName11(const std::string &name, std::string &out, std::wstring &error)
		{
			size_t dot = name.find('.');
			std::string base = dot == std::string::npos ? name : name.substr(0, dot);
			std::string ext  = dot == std::string::npos ? std::string() : name.substr(dot + 1);
			if (base.empty() || base.size() > 8 || ext.size() > 3 ||
				name.find('.', dot == std::string::npos ? 0 : dot + 1) != std::string::npos)
			{
				error = FormatW(L"%hs is not an 8.3 name", name.c_str());
				return false;
			}
			const std::string allowed =
					"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&";
			for (char &c: base) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
			for (char &c: ext) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
			for (char c: base + ext)
			{
				if (allowed.find(c) == std::string::npos)
				{
					error = FormatW(
						L"%hs cannot be expressed as an 8.3 name",
						name.c_str());
					return false;
				}
			}
			out.assign(11, ' ');
			std::copy(base.begin(), base.end(), out.begin());
			std::copy(ext.begin(), ext.end(), out.begin() + 8);
			return true;
		}

		// 查目录的名字键（FatLookupKey 定义在 fat.h）
		bool MakeLookupKey(const std::string &component, FatLookupKey &key, std::wstring &error)
		{
			std::string name11;
			std::wstring ignored;
			if (MakeName11(component, name11, ignored))
			{
				key.useShort = true;
				key.short11  = name11;
				return true;
			}
			if (component.empty())
			{
				error = L"empty path component";
				return false;
			}
			key.useShort  = false;
			key.longName  = Widen(component);
			return true;
		}

		// LFN 条目里的 13 个 UTF-16 字符位置（规范定义的三段）
		void LfnChars(const uint8_t *e, uint16_t out[13])
		{
			int k = 0;
			// 三段各 5/6/2 个 UTF-16：第一段 0x01..0x0A 是 5 个，步长必须是 2
			// （漏掉步长会写/读成 10 个重叠字符，attr 与后半段名字全被踩坏）
			for (int i = 0x01; i <= 0x0A; i += 2) out[k++] = RdU16(e + i);
			for (int i = 0x0E; i <= 0x19; i += 2) out[k++] = RdU16(e + i);
			for (int i = 0x1C; i <= 0x1F; i += 2) out[k++] = RdU16(e + i);
		}

		// LFN 集合的有效性校验：各条目的校验字节必须等于短名的校验和（规范 6.2）
		uint8_t ShortNameChecksum(const uint8_t *name11)
		{
			uint8_t sum = 0;
			for (int i = 0; i < 11; ++i)
				sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
			return sum;
		}

		// 把 13 个字符写进 LFN 条目的三段位置
		void WriteLfnChars(uint8_t *e, const uint16_t in[13])
		{
			int k = 0;
			for (int i = 0x01; i <= 0x0A; i += 2) WrU16(e + i, in[k++]);
			for (int i = 0x0E; i <= 0x19; i += 2) WrU16(e + i, in[k++]);
			for (int i = 0x1C; i <= 0x1F; i += 2) WrU16(e + i, in[k++]);
		}

		// 长名 → 候选短名（"Microsoft" → "MICROS~1"）：基名可用的前若干字符 + "~N"，扩展名取前 3。
		bool MakeShortCandidate(const std::string &name, unsigned seq, std::string &out11, std::wstring &error)
		{
			const size_t dot  = name.rfind('.');
			std::string  base = dot == std::string::npos ? name : name.substr(0, dot);
			std::string  ext  = dot == std::string::npos ? std::string() : name.substr(dot + 1);
			if (ext.size() > 3) ext = ext.substr(0, 3);

			std::string clean;
			for (char c: base)
			{
				const char u = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
				if (std::isalnum(static_cast<unsigned char>(u))) clean.push_back(u);
			}
			if (clean.empty()) clean = "FILE";
			const std::string tilde = "~" + std::to_string(seq);
			if (clean.size() + tilde.size() > 8)
				clean = clean.substr(0, 8 - tilde.size());
			clean += tilde;
			return MakeName11(
				dot == std::string::npos ? clean : clean + "." + ext,
				out11,
				error);
		}

		void DosDateTime(uint16_t &date, uint16_t &time, uint8_t &tenth)
		{
			SYSTEMTIME st{};
			::GetLocalTime(&st);
			const unsigned y = st.wYear < 1980 ? 1980 : st.wYear;
			date  = static_cast<uint16_t>(((y - 1980) << 9) | (st.wMonth << 5) | st.wDay);
			time  = static_cast<uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
			tenth = static_cast<uint8_t>(st.wMilliseconds / 10);
		}

		bool IsPowerOfTwo(uint32_t v)
		{
			return v != 0 && (v & (v - 1)) == 0;
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

	// ---------------------------------------------------------------- FatFileSystem

	bool FatFileSystem::Mount(SectorIo &io, uint64_t partitionBase, std::wstring &error)
	{
		io_      = &io;
		base_    = partitionBase;
		mounted_ = false;

		std::vector<uint8_t> bpb(kSector);
		if (!io.Read(base_, bpb.data(), kSector))
		{
			error = L"cannot read the boot sector";
			return false;
		}
		if (!((bpb[0] == 0xEB && bpb[2] == 0x90) || bpb[0] == 0xE9))
		{
			error = L"no x86 jump instruction at offset 0: not a FAT volume?";
			return false;
		}

		bpb_.bytesPerSector    = RdU16(&bpb[0x0B]);
		bpb_.sectorsPerCluster = bpb[0x0D];
		bpb_.reservedSectors   = RdU16(&bpb[0x0E]);
		bpb_.numFats           = bpb[0x10];
		bpb_.rootEntries       = RdU16(&bpb[0x11]);
		const uint16_t total16 = RdU16(&bpb[0x13]);
		const uint16_t fat16   = RdU16(&bpb[0x16]);
		bpb_.totalSectors      = total16 != 0 ? total16 : RdU32(&bpb[0x20]);
		const uint32_t fat32   = RdU32(&bpb[0x24]);
		bpb_.extFlags          = RdU16(&bpb[0x28]);
		bpb_.rootCluster       = RdU32(&bpb[0x2C]);
		bpb_.fsinfoSector      = RdU16(&bpb[0x30]);

		if (bpb_.bytesPerSector != kSector)
		{
			error = FormatW(
				L"unsupported sector size %u (this writer only handles 512)",
				bpb_.bytesPerSector);
			return false;
		}
		if (!IsPowerOfTwo(bpb_.sectorsPerCluster) || bpb_.sectorsPerCluster == 0 ||
			bpb_.sectorsPerCluster > 128)
		{
			error = FormatW(L"bad sectors-per-cluster %u", bpb_.sectorsPerCluster);
			return false;
		}
		if (bpb_.numFats == 0 || bpb_.reservedSectors == 0 || bpb_.totalSectors == 0)
		{
			error = L"boot sector geometry is incomplete";
			return false;
		}
		bpb_.fatSizeSectors = fat16 != 0 ? fat16 : fat32;
		if (bpb_.fatSizeSectors == 0)
		{
			error = L"FAT size is zero";
			return false;
		}

		bpb_.fatStartSector  = bpb_.reservedSectors;
		bpb_.rootDirSectors  =
				(bpb_.rootEntries * 32 + bpb_.bytesPerSector - 1) / bpb_.bytesPerSector;
		bpb_.rootDirSector   = bpb_.fatStartSector + bpb_.numFats * bpb_.fatSizeSectors;
		bpb_.dataStartSector = bpb_.rootDirSector + bpb_.rootDirSectors;

		const uint32_t dataSectors = bpb_.totalSectors > bpb_.dataStartSector
				? bpb_.totalSectors - bpb_.dataStartSector
				: 0;
		bpb_.clusterCount = dataSectors / bpb_.sectorsPerCluster;
		if (bpb_.clusterCount == 0)
		{
			error = L"no data clusters: geometry is inconsistent";
			return false;
		}

		if (fat16 == 0 && bpb_.rootEntries == 0)
		{
			bpb_.fatBits = 32;
			if (bpb_.rootCluster < 2 || bpb_.rootCluster > bpb_.clusterCount + 1)
			{
				error = L"FAT32 root cluster is out of range";
				return false;
			}
		}
		else if (bpb_.clusterCount < 4085)
		{
			error =
					L"this volume is FAT12; this writer deliberately does not touch FAT12 "
					L"(the ESP is FAT16/FAT32)";
			return false;
		}
		else
		{
			bpb_.fatBits = 16;
		}

		mounted_ = true;
		return true;
	}

	const wchar_t *FatFileSystem::type() const
	{
		return bpb_.fatBits == 32 ? L"FAT32" : L"FAT16";
	}

	uint64_t FatFileSystem::clusterBytes() const
	{
		return static_cast<uint64_t>(bpb_.bytesPerSector) * bpb_.sectorsPerCluster;
	}

	uint64_t FatFileSystem::clusterOffset(uint32_t cluster) const
	{
		const uint64_t sector =
				bpb_.dataStartSector + static_cast<uint64_t>(cluster - 2) * bpb_.sectorsPerCluster;
		return sector * bpb_.bytesPerSector;
	}

	uint64_t FatFileSystem::volumeBytes() const
	{
		return static_cast<uint64_t>(bpb_.totalSectors) * bpb_.bytesPerSector;
	}

	uint64_t FatFileSystem::clusterSize() const
	{
		return clusterBytes();
	}

	bool FatFileSystem::readFat(uint32_t cluster, uint32_t &value, std::wstring &error)
	{
		if (cluster < 2 || cluster > bpb_.clusterCount + 1)
		{
			error = FormatW(L"cluster %u is out of range", cluster);
			return false;
		}
		if (bpb_.fatBits == 32)
		{
			uint8_t raw[4];
			const uint64_t offset =
					static_cast<uint64_t>(bpb_.fatStartSector) * bpb_.bytesPerSector +
					static_cast<uint64_t>(cluster) * 4;
			if (!io_->Read(base_ + offset, raw, 4))
			{
				error = L"cannot read the FAT";
				return false;
			}
			value = RdU32(raw) & 0x0FFFFFFFu;
			return true;
		}
		uint8_t raw[2];
		const uint64_t offset = static_cast<uint64_t>(bpb_.fatStartSector) * bpb_.bytesPerSector +
				static_cast<uint64_t>(cluster) * 2;
		if (!io_->Read(base_ + offset, raw, 2))
		{
			error = L"cannot read the FAT";
			return false;
		}
		value = RdU16(raw);
		return true;
	}

	bool FatFileSystem::writeSector(uint64_t offset, const void *data, size_t len, std::wstring &error)
	{
		if (journal_ != nullptr && !journal_->Capture(*io_, base_ + offset, len, error))
			return false;
		if (!io_->Write(base_ + offset, data, len))
		{
			error = FormatW(
				L"cannot write at %llu",
				static_cast<unsigned long long>(base_ + offset));
			return false;
		}
		return true;
	}

	bool FatFileSystem::writeFat(uint32_t cluster, uint32_t value, std::wstring &error)
	{
		// FAT32 的表项高 4 位是保留位：原样保留（规范要求）
		uint32_t v = value;
		if (bpb_.fatBits == 32) v &= 0x0FFFFFFFu;

		// 镜像全部 FAT 副本；只有显式关闭镜像（EXTFLAGS 位 7）时才只写活动副本
		uint32_t activeOnly = 0;
		const bool mirrored = (bpb_.extFlags & 0x0080u) == 0;
		if (!mirrored) activeOnly = bpb_.extFlags & 0x000Fu;

		for (uint32_t fat = 0; fat < bpb_.numFats; ++fat)
		{
			if (!mirrored && fat != activeOnly) continue;
			const uint64_t fatBase = static_cast<uint64_t>(bpb_.fatStartSector +
			                                               fat * bpb_.fatSizeSectors) *
					bpb_.bytesPerSector;
			if (bpb_.fatBits == 32)
			{
				const uint64_t offset = fatBase + static_cast<uint64_t>(cluster) * 4;
				uint8_t        raw[4];
				if (!io_->Read(base_ + offset, raw, 4))
				{
					error = L"cannot read the FAT entry";
					return false;
				}
				WrU32(raw, (RdU32(raw) & 0xF0000000u) | v);
				if (!writeSector(offset, raw, 4, error)) return false;
			}
			else
			{
				const uint64_t offset = fatBase + static_cast<uint64_t>(cluster) * 2;
				uint8_t        raw[2];
				WrU16(raw, static_cast<uint16_t>(v));
				if (!writeSector(offset, raw, 2, error)) return false;
			}
		}
		return true;
	}

	uint64_t FatFileSystem::freeBytes() const
	{
		if (!mounted_) return 0;
		uint64_t free = 0;
		for (uint32_t c = 2; c <= bpb_.clusterCount + 1; ++c)
		{
			uint32_t       value = 0;
			std::wstring   ignored;
			FatFileSystem *self = const_cast<FatFileSystem *>(this);
			if (self->readFat(c, value, ignored) && value == 0) ++free;
		}
		return free * clusterBytes();
	}

	bool FatFileSystem::allocateChain(uint32_t count, uint32_t &first, std::wstring &error)
	{
		first = 0;
		if (count == 0) return true;

		std::vector<uint32_t> picked;
		picked.reserve(count);
		for (uint32_t c = 2; c <= bpb_.clusterCount + 1 && picked.size() < count; ++c)
		{
			uint32_t value = 0;
			if (!readFat(c, value, error)) return false;
			if (value != 0) continue;
			if (strictAlloc_ && value != 0) // 冗余断言：只走空闲簇
			{
				error = FormatW(L"cluster %u is not free", c);
				return false;
			}
			picked.push_back(c);
		}
		if (picked.size() < count)
		{
			error = FormatW(
				L"not enough free clusters: need %u, found %zu",
				count,
				picked.size());
			return false;
		}

		for (size_t i = 0; i < picked.size(); ++i)
		{
			const uint32_t next = (i + 1 < picked.size())
					? picked[i + 1]
					: (bpb_.fatBits == 32 ? 0x0FFFFFFFu : 0xFFFFu);
			if (!writeFat(picked[i], next, error)) return false;
		}
		first = picked.front();
		return true;
	}

	bool FatFileSystem::freeChain(uint32_t first, std::wstring &error)
	{
		uint32_t cluster = first;
		uint32_t guard   = 0;
		while (cluster >= 2 && cluster <= bpb_.clusterCount + 1)
		{
			uint32_t next = 0;
			if (!readFat(cluster, next, error)) return false;
			if (!writeFat(cluster, 0, error)) return false;
			if (next >= 0xFFF8u && bpb_.fatBits == 16) break;
			if (next >= 0x0FFFFFF8u) break;
			cluster = next;
			if (++guard > bpb_.clusterCount + 2)
			{
				error = L"cluster chain loops: refusing to continue";
				return false;
			}
		}
		return true;
	}

	bool FatFileSystem::forEachDirSlot(
		const DirEntry &                                                  dir,
		const std::function<bool(uint32_t offset, const uint8_t *entry)> &fn,
		std::wstring &                                                    error)
	{
		// FAT16 根目录是固定区，没有簇链
		if (dir.firstCluster == 0 && bpb_.fatBits == 16)
		{
			const uint64_t bytes = static_cast<uint64_t>(bpb_.rootEntries) * 32;
			std::vector<uint8_t> buf(static_cast<size_t>(bytes));
			const uint64_t offset = static_cast<uint64_t>(bpb_.rootDirSector) * bpb_.bytesPerSector;
			if (!io_->Read(base_ + offset, buf.data(), buf.size()))
			{
				error = L"cannot read the FAT16 root directory";
				return false;
			}
			for (uint32_t i = 0; i < bpb_.rootEntries; ++i)
			{
				if (!fn(static_cast<uint32_t>(offset + i * 32), &buf[i * 32])) return true;
			}
			return true;
		}

		uint32_t cluster = dir.firstCluster;
		uint32_t guard   = 0;
		while (cluster >= 2 && cluster <= bpb_.clusterCount + 1)
		{
			const uint64_t offset  = clusterOffset(cluster);
			const size_t   entries = static_cast<size_t>(clusterBytes() / 32);
			std::vector<uint8_t> buf(static_cast<size_t>(clusterBytes()));
			if (!io_->Read(base_ + offset, buf.data(), buf.size()))
			{
				error = L"cannot read a directory cluster";
				return false;
			}
			for (size_t i = 0; i < entries; ++i)
			{
				if (!fn(static_cast<uint32_t>(offset + i * 32), &buf[i * 32])) return true;
			}
			uint32_t next = 0;
			if (!readFat(cluster, next, error)) return false;
			if ((bpb_.fatBits == 16 && next >= 0xFFF8u) || next >= 0x0FFFFFF8u) break;
			cluster = next;
			if (++guard > bpb_.clusterCount + 2)
			{
				error = L"directory chain loops";
				return false;
			}
		}
		return true;
	}

	bool FatFileSystem::findInDir(
		const DirEntry &    dir,
		const FatLookupKey & key,
		DirEntry &          out,
		bool &              found,
		uint32_t *          freeSlotOffset,
		std::wstring &      error)
	{
		found = false;
		uint32_t firstFree = 0;

		// LFN 组装状态：条目物理顺序是 [N|0x40][N-1]…[1][短名]，
		// 所以按 order 字段回填到 (order-1)*13 的位置即可还原名字。
		std::vector<uint16_t> lfnChars;
		bool                  lfnValid    = false;
		uint8_t               lfnChecksum = 0;

		const bool ok = forEachDirSlot(
			dir,
			[&](uint32_t offset, const uint8_t *entry) -> bool
			{
				const uint8_t first = entry[0];
				if (first == 0x00)
				{
					if (firstFree == 0) firstFree = offset; // 终止符本身可复用
					return false;
				}
				if (first == 0xE5)
				{
					if (firstFree == 0) firstFree = offset;
					lfnChars.clear();
					lfnValid = false;
					return true;
				}
				const uint8_t attr = entry[0x0B];
				if ((attr & 0x3F) == 0x0F)
				{
					// LFN 条目：按序号回填
					const uint32_t order = first & 0x3F;
					if (order == 0 || order > 20)
					{
						lfnChars.clear();
						lfnValid = false;
						return true;
					}
					if (lfnChars.size() < order * 13) lfnChars.resize(order * 13, 0);
					uint16_t chars[13];
					LfnChars(entry, chars);
					std::copy(chars, chars + 13, lfnChars.begin() + (order - 1) * 13);
					// 校验存在 **LFN 条目自己的** 0x0D 里（0x0C 是 type）—— 要与短名算出的
					// 校验和比对，而不是读短名条目的 0x0D（那是创建时间十位）。
					lfnChecksum = entry[0x0D];
					lfnValid    = true;
					return true;
				}

				// 短名条目：先把 LFN 结果拼出来（校验和不符的 LFN 集合视为无效）
				std::wstring longName;
				if (lfnValid && lfnChecksum == ShortNameChecksum(entry))
				{
					{
						for (uint16_t c: lfnChars)
						{
							if (c == 0x0000 || c == 0xFFFF) break;
							longName.push_back(static_cast<wchar_t>(c));
						}
					}
				}
				lfnChars.clear();
				lfnValid = false;

				bool match = false;
				if (key.useShort)
				{
					match = std::memcmp(entry, key.short11.data(), 11) == 0;
				}
				else if (!longName.empty())
				{
					match = _wcsicmp(longName.c_str(), key.longName.c_str()) == 0;
				}
				if (!match) return true;

				out.offset       = offset;
				out.attr         = attr;
				out.firstCluster = static_cast<uint32_t>(RdU16(&entry[0x1A])) |
						(static_cast<uint32_t>(RdU16(&entry[0x14])) << 16);
				out.size = RdU32(&entry[0x1C]);
				if (bpb_.fatBits == 16) out.firstCluster = RdU16(&entry[0x1A]);
				found = true;
				return false;
			},
			error);
		if (!ok) return false;
		if (freeSlotOffset != nullptr) *freeSlotOffset = found ? 0 : firstFree;
		return true;
	}

	bool FatFileSystem::resolveDir(
		const std::string &dirPath,
		bool               create,
		DirEntry &         out,
		std::wstring &     error)
	{
		out.firstCluster = bpb_.fatBits == 32 ? bpb_.rootCluster : 0;
		out.attr         = 0x10;
		out.offset       = bpb_.fatBits == 16
				? static_cast<uint32_t>(bpb_.rootDirSector * bpb_.bytesPerSector)
				: 0;
		if (dirPath.empty() || dirPath == ".") return true;

		size_t pos = 0;
		while (pos <= dirPath.size())
		{
			const size_t slash = dirPath.find('\\', pos);
			const std::string part = dirPath.substr(
				pos,
				slash == std::string::npos ? std::string::npos : slash - pos);
			pos = slash == std::string::npos ? dirPath.size() + 1 : slash + 1;
			if (part.empty()) continue;

			FatLookupKey key;
			if (!MakeLookupKey(part, key, error)) return false;

			DirEntry child;
			bool     found      = false;
			uint32_t freeSlot   = 0;
			if (!findInDir(out, key, child, found, &freeSlot, error)) return false;

			if (!found)
			{
				if (!create)
				{
					error = FormatW(L"directory component %hs is missing", part.c_str());
					return false;
				}
				// 建目录：一个簇 + "." / ".." 两个条目
				std::string name11;
				const bool  shortOk = MakeName11(part, name11, error);
				error.clear();
				uint32_t cluster = 0;
				std::vector<uint8_t> zero(static_cast<size_t>(clusterBytes()), 0);
				const bool oldStrict = setStrictAllocation(false);
				const bool allocated = allocateChain(1, cluster, error);
				setStrictAllocation(oldStrict);
				if (!allocated) return false;
				if (!writeSector(clusterOffset(cluster), zero.data(), zero.size(), error))
					return false;

				uint16_t date = 0, time = 0;
				uint8_t  tenth = 0;
				DosDateTime(date, time, tenth);
				uint8_t dot[32] = {};
				std::memset(dot, ' ', 11);
				dot[0]  = '.';
				dot[0x0B] = 0x10;
				WrU16(dot + 0x14, static_cast<uint16_t>(cluster >> 16));
				WrU16(dot + 0x1A, static_cast<uint16_t>(cluster & 0xFFFF));
				WrU16(dot + 0x16, time);
				WrU16(dot + 0x18, date);
				if (!writeSector(clusterOffset(cluster), dot, 32, error)) return false;

				uint8_t dotdot[32] = {};
				std::memset(dotdot, ' ', 11);
				dotdot[0]     = '.';
				dotdot[1]     = '.';
				dotdot[0x0B]  = 0x10;
				// FAT16 下父目录是根 → ".." 的簇号写 0（规范约定）
				const uint32_t parentCluster =
						(out.firstCluster == 0) ? 0 : out.firstCluster;
				WrU16(dotdot + 0x14, static_cast<uint16_t>(parentCluster >> 16));
				WrU16(dotdot + 0x1A, static_cast<uint16_t>(parentCluster & 0xFFFF));
				WrU16(dotdot + 0x16, time);
				WrU16(dotdot + 0x18, date);
				if (!writeSector(clusterOffset(cluster) + 32, dotdot, 32, error)) return false;

				// 父目录里落一条目录条目（名字超 8.3 就带 LFN 条目）
				if (!shortOk)
				{
					if (!writeLongEntry(out, part, 0x10, cluster, 0, error)) return false;
				}
				else
				{
					if (freeSlot == 0)
					{
						error = FormatW(L"directory %hs has no free slot", dirPath.c_str());
						return false;
					}
					uint8_t e[32] = {};
					std::memcpy(e, name11.data(), 11);
					e[0x0B] = 0x10;
					WrU16(e + 0x14, static_cast<uint16_t>(cluster >> 16));
					WrU16(e + 0x1A, static_cast<uint16_t>(cluster & 0xFFFF));
					WrU16(e + 0x16, time);
					WrU16(e + 0x18, date);
					if (!writeSector(freeSlot, e, 32, error)) return false;
				}

				child.offset       = 0;
				child.attr         = 0x10;
				child.firstCluster = cluster;
				child.size         = 0;
			}
			else if ((child.attr & 0x10) == 0)
			{
				error = FormatW(L"%hs exists but is not a directory", part.c_str());
				return false;
			}
			out = child;
		}
		return true;
	}

	bool FatFileSystem::findFreeRun(
		const DirEntry &dir,
		uint32_t        slots,
		uint32_t &      offset,
		std::wstring &  error)
	{
		offset = 0;
		bool     found      = false;
		uint32_t runStart   = 0;
		uint32_t runLength  = 0;
		uint32_t lastOffset = 0;

		const bool ok = forEachDirSlot(
			dir,
			[&](uint32_t slotOffset, const uint8_t *entry) -> bool
			{
				lastOffset = slotOffset;
				const uint8_t first = entry[0];
				const bool    free  = (first == 0xE5) || (first == 0x00);
				if (free)
				{
					if (runLength == 0) runStart = slotOffset;
					++runLength;
					if (runLength >= slots)
					{
						offset = runStart;
						found  = true;
						return false;
					}
				}
				else
				{
					runLength = 0;
				}
				return true;
			},
			error);
		if (!ok) return false;
		if (found) return true;

		// 目录满了：FAT32/子目录可以再挂一个簇（FAT16 根目录是固定区，无解）
		if (dir.firstCluster == 0 && bpb_.fatBits == 16)
		{
			error = L"directory is full (FAT16 root area cannot grow)";
			return false;
		}
		uint32_t lastCluster = dir.firstCluster;
		uint32_t guard       = 0;
		while (true)
		{
			uint32_t next = 0;
			if (!readFat(lastCluster, next, error)) return false;
			if ((bpb_.fatBits == 16 && next >= 0xFFF8u) || next >= 0x0FFFFFF8u) break;
			lastCluster = next;
			if (++guard > bpb_.clusterCount + 2)
			{
				error = L"directory chain loops";
				return false;
			}
		}
		uint32_t fresh = 0;
		if (!allocateChain(1, fresh, error)) return false;
		std::vector<uint8_t> zero(static_cast<size_t>(clusterBytes()), 0);
		if (!writeSector(clusterOffset(fresh), zero.data(), zero.size(), error)) return false;
		if (!writeFat(lastCluster, bpb_.fatBits == 32 ? 0x0FFFFFFFu : 0xFFFFu, error))
			return false;
		offset = static_cast<uint32_t>(clusterOffset(fresh));
		(void) lastOffset;
		return true;
	}

	bool FatFileSystem::writeLongEntry(
		const DirEntry &    dir,
		const std::string & longName,
		uint8_t             attr,
		uint32_t            firstCluster,
		uint64_t            size,
		std::wstring &      error)
	{
		const std::wstring wide = Widen(longName);
		const uint32_t     lfnCount =
				static_cast<uint32_t>((wide.size() + 12) / 13);
		if (lfnCount == 0 || lfnCount > 20)
		{
			error = FormatW(L"%hs is too long for an LFN name", longName.c_str());
			return false;
		}

		// 唯一短名：MICROS~1 / MICROS~2 / …（碰撞时递增）
		std::string shortName11;
		bool        unique = false;
		for (unsigned seq = 1; seq <= 100000 && !unique; ++seq)
		{
			if (!MakeShortCandidate(longName, seq, shortName11, error)) return false;
			FatLookupKey key;
			key.useShort = true;
			key.short11  = shortName11;
			DirEntry clash;
			bool     clashFound = false;
			if (!findInDir(dir, key, clash, clashFound, nullptr, error)) return false;
			unique = !clashFound;
		}
		if (!unique)
		{
			error = FormatW(L"cannot find a unique short name for %hs", longName.c_str());
			return false;
		}

		uint32_t run = 0;
		if (!findFreeRun(dir, lfnCount + 1, run, error)) return false;

		const uint8_t checksum = ShortNameChecksum(
			reinterpret_cast<const uint8_t *>(shortName11.data()));

		// LFN 条目：物理顺序是 [N|0x40][N-1]…[1]，随后跟短名条目
		for (uint32_t i = lfnCount; i >= 1; --i)
		{
			uint8_t e[32] = {};
			e[0]   = static_cast<uint8_t>(i | (i == lfnCount ? 0x40 : 0x00));
			e[0x0B] = 0x0F;
			e[0x0C] = 0x00;
			e[0x0D] = checksum;
			uint16_t chars[13];
			for (int k = 0; k < 13; ++k)
			{
				const size_t index = static_cast<size_t>(i - 1) * 13 + k;
				if (index < wide.size()) chars[k] = static_cast<uint16_t>(wide[index]);
				else if (index == wide.size()) chars[k] = 0x0000;
				else chars[k] = 0xFFFF;
			}
			WriteLfnChars(e, chars);
			const uint32_t at = run + (lfnCount - i) * 32;
			if (!writeSector(at, e, 32, error)) return false;
		}

		uint16_t date = 0, time = 0;
		uint8_t  tenth = 0;
		DosDateTime(date, time, tenth);
		uint8_t e[32] = {};
		std::memcpy(e, shortName11.data(), 11);
		e[0x0B] = attr;
		WrU16(e + 0x14, static_cast<uint16_t>(firstCluster >> 16));
		WrU16(e + 0x1A, static_cast<uint16_t>(firstCluster & 0xFFFF));
		WrU16(e + 0x16, time);
		WrU16(e + 0x18, date);
		WrU32(e + 0x1C, static_cast<uint32_t>(size));
		if (bpb_.fatBits == 16) WrU16(e + 0x14, 0);
		return writeSector(run + lfnCount * 32, e, 32, error);
	}

	bool FatFileSystem::Exists(const std::string &shortPath, uint64_t *sizeOut, bool *isDir, std::wstring &error)
	{
		const size_t slash = shortPath.rfind('\\');
		const std::string dirPath = slash == std::string::npos ? std::string() : shortPath.substr(0, slash);
		const std::string name    = slash == std::string::npos ? shortPath : shortPath.substr(slash + 1);

		FatLookupKey key;
		if (!MakeLookupKey(name, key, error)) return false;

		DirEntry dir;
		if (!resolveDir(dirPath, /*create=*/false, dir, error)) return false;
		DirEntry entry;
		bool     found = false;
		if (!findInDir(dir, key, entry, found, nullptr, error)) return false;
		if (!found) return false;
		if (sizeOut != nullptr) *sizeOut = entry.size;
		if (isDir != nullptr) *isDir = (entry.attr & 0x10) != 0;
		return true;
	}

	bool FatFileSystem::MakeDirectory(const std::string &shortPath, std::wstring &error)
	{
		DirEntry dir;
		return resolveDir(shortPath, /*create=*/true, dir, error);
	}

	bool FatFileSystem::WriteFile(
		const std::string &                                   shortPath,
		uint64_t                                              size,
		const std::function<bool(uint64_t, void *, size_t)> & reader,
		std::wstring &                                        error)
	{
		const size_t slash = shortPath.rfind('\\');
		const std::string dirPath = slash == std::string::npos ? std::string() : shortPath.substr(0, slash);
		const std::string name    = slash == std::string::npos ? shortPath : shortPath.substr(slash + 1);

		FatLookupKey key;
		if (!MakeLookupKey(name, key, error)) return false;

		DirEntry dir;
		if (!resolveDir(dirPath, /*create=*/true, dir, error)) return false;

		DirEntry existing;
		bool     found    = false;
		uint32_t freeSlot = 0;
		if (!findInDir(dir, key, existing, found, &freeSlot, error)) return false;
		// 新建条目：名字表达不成 8.3 时走 LFN；替换已有条目不改名、不需要 11 字节字段
		std::string name11;
		bool        shortOk = true;
		if (!found)
		{
			shortOk = MakeName11(name, name11, error);
			error.clear(); // 长名不是错误：下面走 LFN 分支
		}
		if (found && (existing.attr & 0x10) != 0)
		{
			error = FormatW(L"%hs exists and is a directory", shortPath.c_str());
			return false;
		}

		const uint32_t clusters = static_cast<uint32_t>(
			(size + clusterBytes() - 1) / clusterBytes());
		uint32_t first = 0;
		if (clusters > 0 && !allocateChain(clusters, first, error)) return false;

		// 数据写在目录条目生效之前：中途崩了也只是"新链已分配、旧文件仍在"
		std::vector<uint8_t> buf(static_cast<size_t>(clusterBytes()));
		uint32_t             cluster = first;
		for (uint32_t i = 0; i < clusters; ++i)
		{
			const uint64_t done = static_cast<uint64_t>(i) * clusterBytes();
			const size_t   take = static_cast<size_t>(
				std::min<uint64_t>(clusterBytes(), size - done));
			std::fill(buf.begin(), buf.end(), 0);
			if (take > 0 && !reader(done, buf.data(), take))
			{
				error = FormatW(
					L"cannot read the source of %hs at offset %llu",
					shortPath.c_str(),
					static_cast<unsigned long long>(done));
				return false;
			}
			// 数据簇属于"本来空闲"的空间：不写日志（见 fat.h 的说明）
			if (!io_->Write(base_ + clusterOffset(cluster), buf.data(), buf.size()))
			{
				error = L"cannot write file data";
				return false;
			}
			if (i + 1 < clusters)
			{
				uint32_t next = 0;
				if (!readFat(cluster, next, error)) return false;
				cluster = next;
			}
		}

		uint16_t date = 0, time = 0;
		uint8_t  tenth = 0;
		DosDateTime(date, time, tenth);

		// 新建 + 名字表达不成 8.3 → 用 LFN 条目建（老条目在下面统一处理）
		if (!found && !shortOk)
		{
			if (!writeLongEntry(dir, name, 0x20, first, size, error)) return false;
			return flushFsInfo(error);
		}

		const uint32_t slot = found ? existing.offset : freeSlot;
		if (slot == 0)
		{
			error = FormatW(L"no free directory slot for %hs", shortPath.c_str());
			return false;
		}
		uint8_t e[32] = {};
		if (!io_->Read(base_ + slot, e, 32))
		{
			error = L"cannot read the directory entry";
			return false;
		}
		if (found)
		{
			// 替换已有条目：名字（可能是 LFN 的短名形式）与属性原样保留，只改簇号/大小/时间
			e[0x0B] = static_cast<uint8_t>(e[0x0B] & ~0x01); // 清掉只读位（文件要被换掉）
		}
		else
		{
			std::memcpy(e, name11.data(), 11);
			e[0x0B] = 0x20; // archive
		}
		WrU16(e + 0x14, static_cast<uint16_t>(first >> 16));
		WrU16(e + 0x1A, static_cast<uint16_t>(first & 0xFFFF));
		WrU16(e + 0x16, time);
		WrU16(e + 0x18, date);
		WrU32(e + 0x1C, static_cast<uint32_t>(size));
		if (bpb_.fatBits == 16) WrU16(e + 0x14, 0);
		if (!writeSector(slot, e, 32, error)) return false;

		// 旧链在条目生效之后才释放：中途崩了也只是旧链泄漏
		if (found && existing.firstCluster >= 2 && existing.firstCluster != first)
		{
			if (!freeChain(existing.firstCluster, error)) return false;
		}
		return flushFsInfo(error);
	}

	bool FatFileSystem::ReadFile(const std::string &shortPath, std::vector<uint8_t> &out, std::wstring &error)
	{
		uint64_t size = 0;
		if (!Exists(shortPath, &size, nullptr, error)) return false;

		const size_t slash = shortPath.rfind('\\');
		const std::string dirPath = slash == std::string::npos ? std::string() : shortPath.substr(0, slash);
		const std::string name    = slash == std::string::npos ? shortPath : shortPath.substr(slash + 1);
		FatLookupKey key;
		if (!MakeLookupKey(name, key, error)) return false;
		DirEntry dir;
		if (!resolveDir(dirPath, false, dir, error)) return false;
		DirEntry entry;
		bool     found = false;
		if (!findInDir(dir, key, entry, found, nullptr, error) || !found)
		{
			error = FormatW(L"%hs not found", shortPath.c_str());
			return false;
		}

		out.assign(static_cast<size_t>(size), 0);
		uint64_t done    = 0;
		uint32_t cluster = entry.firstCluster;
		while (done < size && cluster >= 2 && cluster <= bpb_.clusterCount + 1)
		{
			const size_t take = static_cast<size_t>(
				std::min<uint64_t>(clusterBytes(), size - done));
			if (!io_->Read(base_ + clusterOffset(cluster), out.data() + done, take))
			{
				error = L"cannot read file data";
				return false;
			}
			done += take;
			uint32_t next = 0;
			if (!readFat(cluster, next, error)) return false;
			if ((bpb_.fatBits == 16 && next >= 0xFFF8u) || next >= 0x0FFFFFF8u) break;
			cluster = next;
		}
		if (done != size)
		{
			error = FormatW(
				L"%hs is shorter than its directory entry claims (%llu of %llu)",
				shortPath.c_str(),
				static_cast<unsigned long long>(done),
				static_cast<unsigned long long>(size));
			return false;
		}
		return true;
	}

	bool FatFileSystem::VerifyFile(
		const std::string &                                   shortPath,
		uint64_t                                              size,
		const std::function<bool(uint64_t, void *, size_t)> & reader,
		std::wstring &                                        error)
	{
		std::vector<uint8_t> onDisk;
		if (!ReadFile(shortPath, onDisk, error)) return false;
		if (onDisk.size() != size)
		{
			error = FormatW(
				L"%hs: size on disk is %zu, expected %llu",
				shortPath.c_str(),
				onDisk.size(),
				static_cast<unsigned long long>(size));
			return false;
		}
		std::vector<uint8_t> expected(static_cast<size_t>(std::min<uint64_t>(size, 64 * 1024)));
		for (uint64_t done = 0; done < size;)
		{
			const size_t take = static_cast<size_t>(
				std::min<uint64_t>(expected.size(), size - done));
			if (!reader(done, expected.data(), take))
			{
				error = FormatW(L"cannot re-read the source of %hs", shortPath.c_str());
				return false;
			}
			if (std::memcmp(expected.data(), onDisk.data() + done, take) != 0)
			{
				error = FormatW(
					L"%hs: content differs at offset %llu",
					shortPath.c_str(),
					static_cast<unsigned long long>(done));
				return false;
			}
			done += take;
		}
		return true;
	}

	void FatFileSystem::SetJournal(UndoJournal *journal)
	{
		journal_ = journal;
	}

	bool FatFileSystem::flushFsInfo(std::wstring &error)
	{
		if (bpb_.fatBits != 32 || bpb_.fsinfoSector == 0) return true;
		const uint64_t offset = static_cast<uint64_t>(bpb_.fsinfoSector) * bpb_.bytesPerSector;
		uint8_t        sector[kSector];
		if (!io_->Read(base_ + offset, sector, kSector)) return true; // 没有 FSInfo 也不算错
		if (RdU32(sector) != 0x41615252u || RdU32(sector + 0x1E4) != 0x61417272u) return true;

		uint64_t free = 0;
		for (uint32_t c = 2; c <= bpb_.clusterCount + 1; ++c)
		{
			uint32_t value = 0;
			if (!readFat(c, value, error)) return false;
			if (value == 0) ++free;
		}
		WrU32(sector + 0x1E8, static_cast<uint32_t>(free));
		WrU32(sector + 0x1EC, 0xFFFFFFFFu); // "下一个空闲簇未知"：规范的合法取值
		return writeSector(offset, sector, kSector, error);
	}

	// ------------------------------------------------------- 自检用的最小格式化

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
			error = L"test formatter only supports 512-byte sectors";
			return false;
		}
		const uint32_t totalSectors = static_cast<uint32_t>(sizeBytes / bytesPerSector);
		const uint32_t reserved     = 32;
		const uint32_t numFats      = 2;
		const uint32_t rootCluster  = 2;
		const uint32_t dataStart    = reserved; // 先占位，稍后加上 FAT 大小
		(void) dataStart;

		// FAT 表大小：让它足够覆盖全部簇（多算一点无妨）
		const uint32_t approxClusters = totalSectors / sectorsPerCluster + 1;
		uint32_t fatSectors = (approxClusters * 4 + bytesPerSector - 1) / bytesPerSector;
		fatSectors          = (fatSectors + 7) / 8 * 8;

		const uint32_t dataStartFinal = reserved + numFats * fatSectors;
		if (dataStartFinal >= totalSectors)
		{
			error = L"image is too small for this geometry";
			return false;
		}

		std::vector<uint8_t> sector(bytesPerSector, 0);
		// 引导扇区
		sector[0] = 0xEB;
		sector[1] = 0x58;
		sector[2] = 0x90;
		std::memcpy(&sector[3], "ICEMELT ", 8);
		WrU16(&sector[0x0B], static_cast<uint16_t>(bytesPerSector));
		sector[0x0D] = static_cast<uint8_t>(sectorsPerCluster);
		WrU16(&sector[0x0E], static_cast<uint16_t>(reserved));
		sector[0x10] = static_cast<uint8_t>(numFats);
		WrU16(&sector[0x11], 0);
		WrU16(&sector[0x13], 0);
		sector[0x15] = 0xF8;
		WrU16(&sector[0x16], 0);
		WrU16(&sector[0x18], 63);
		WrU16(&sector[0x1A], 255);
		WrU32(&sector[0x1C], 0);
		WrU32(&sector[0x20], totalSectors);
		WrU32(&sector[0x24], fatSectors);
		WrU16(&sector[0x28], 0);
		WrU16(&sector[0x2A], 0);
		WrU32(&sector[0x2C], rootCluster);
		WrU16(&sector[0x30], 1);
		WrU16(&sector[0x32], 6);
		sector[0x40] = 0x80;
		sector[0x42] = 0x29;
		WrU32(&sector[0x43], 0x12345678u);
		std::memcpy(&sector[0x47], "ICEMELT-TEST", 12);
		std::memcpy(&sector[0x52], "FAT32   ", 8);
		sector[0x1FE] = 0x55;
		sector[0x1FF] = 0xAA;
		if (!io.Write(base, sector.data(), sector.size()))
		{
			error = L"cannot write the boot sector";
			return false;
		}
		// FSInfo
		std::vector<uint8_t> fsi(bytesPerSector, 0);
		WrU32(&fsi[0], 0x41615252u);
		WrU32(&fsi[0x1E4], 0x61417272u);
		WrU32(&fsi[0x1E8], 0xFFFFFFFFu);
		WrU32(&fsi[0x1EC], 0xFFFFFFFFu);
		WrU32(&fsi[0x1FC], 0xAA550000u);
		if (!io.Write(base + bytesPerSector, fsi.data(), fsi.size()))
		{
			error = L"cannot write FSInfo";
			return false;
		}
		// FAT：保留 0/1 两条表项，根簇标记 EOC
		for (uint32_t fat = 0; fat < numFats; ++fat)
		{
			std::vector<uint8_t> fatBytes(static_cast<size_t>(fatSectors) * bytesPerSector, 0);
			WrU32(&fatBytes[0], 0x0FFFFFF8u);
			WrU32(&fatBytes[4], 0x0FFFFFFFu);
			// 簇 N 的表项就在 N*4：根簇 = cluster 2 → 偏移 8（曾经写成 8+2*4=16，
			// 那等于把 EOC 标到簇 4 上，根簇看起来是空闲的 —— 分配器于是把新目录
			// 分到了根簇，整棵目录树塌成一层）
			WrU32(&fatBytes[rootCluster * 4], 0x0FFFFFFFu);
			const uint64_t offset =
					static_cast<uint64_t>(reserved + fat * fatSectors) * bytesPerSector;
			if (!io.Write(base + offset, fatBytes.data(), fatBytes.size()))
			{
				error = L"cannot write the FAT";
				return false;
			}
		}
		// 根目录簇清零
		std::vector<uint8_t> zero(static_cast<size_t>(bytesPerSector) * sectorsPerCluster, 0);
		const uint64_t rootOffset =
				static_cast<uint64_t>(dataStartFinal) * bytesPerSector;
		if (!io.Write(base + rootOffset, zero.data(), zero.size()))
		{
			error = L"cannot zero the root directory";
			return false;
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
		if (handle_ == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(offset);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
		DWORD read = 0;
		return ::ReadFile(handle_, dst, static_cast<DWORD>(len), &read, nullptr) &&
				read == len;
	}

	bool FileSectorIo::Write(uint64_t offset, const void *src, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(offset);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
		DWORD written = 0;
		return ::WriteFile(handle_, src, static_cast<DWORD>(len), &written, nullptr) &&
				written == len;
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

	bool PhysicalDriveSectorIo::Read(uint64_t offset, void *dst, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE || len == 0) return false;
		if (aligned(offset, len))
		{
			LARGE_INTEGER li{};
			li.QuadPart = static_cast<LONGLONG>(offset);
			if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
			DWORD read = 0;
			return ::ReadFile(handle_, dst, static_cast<DWORD>(len), &read, nullptr) &&
					read == len;
		}
		// 非对齐：读-改-写覆盖到的整扇区
		const uint64_t first = (offset / kSector) * kSector;
		const uint64_t last  = (offset + len - 1) / kSector * kSector;
		const size_t   total = static_cast<size_t>(last - first + kSector);
		std::vector<uint8_t> buf(total);
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
		DWORD read = 0;
		if (!::ReadFile(handle_, buf.data(), static_cast<DWORD>(total), &read, nullptr) ||
			read != total)
			return false;
		std::memcpy(dst, buf.data() + (offset - first), len);
		return true;
	}

	bool PhysicalDriveSectorIo::Write(uint64_t offset, const void *src, size_t len)
	{
		if (handle_ == INVALID_HANDLE_VALUE || len == 0) return false;
		if (aligned(offset, len))
		{
			LARGE_INTEGER li{};
			li.QuadPart = static_cast<LONGLONG>(offset);
			if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
			DWORD written = 0;
			return ::WriteFile(handle_, src, static_cast<DWORD>(len), &written, nullptr) &&
					written == len;
		}
		const uint64_t first = (offset / kSector) * kSector;
		const uint64_t last  = (offset + len - 1) / kSector * kSector;
		const size_t   total = static_cast<size_t>(last - first + kSector);
		std::vector<uint8_t> buf(total);
		LARGE_INTEGER li{};
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
		DWORD read = 0;
		if (!::ReadFile(handle_, buf.data(), static_cast<DWORD>(total), &read, nullptr) ||
			read != total)
			return false;
		std::memcpy(buf.data() + (offset - first), src, len);
		li.QuadPart = static_cast<LONGLONG>(first);
		if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;
		DWORD written = 0;
		return ::WriteFile(handle_, buf.data(), static_cast<DWORD>(total), &written, nullptr) &&
				written == total;
	}

	// ------------------------------------------------------------ 卷锁/卸载

	bool LockCoveringVolume(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		HANDLE &      volumeHandle,
		std::wstring &error)
	{
		volumeHandle = INVALID_HANDLE_VALUE;
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
			HANDLE h = ::CreateFileW(
				path.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (h == INVALID_HANDLE_VALUE) continue;

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
			// ESP 通常没有挂载点，也就可能查不到卷设备：那就没什么要锁的，裸写本来就允许
			volumeHandle = INVALID_HANDLE_VALUE;
			return true;
		}

		DWORD bytes = 0;
		if (::DeviceIoControl(volumeHandle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr))
		{
			::DeviceIoControl(volumeHandle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
		}
		else
		{
			const DWORD err = ::GetLastError();
			// 158(ERROR_NOT_LOCKED) / 1(ERROR_INVALID_FUNCTION)：没有已挂载的文件系统在管它
			if (err != ERROR_NOT_LOCKED && err != ERROR_INVALID_FUNCTION &&
				err != ERROR_ACCESS_DENIED)
			{
				error = FormatW(L"FSCTL_LOCK_VOLUME failed: %lu", err);
				::CloseHandle(volumeHandle);
				volumeHandle = INVALID_HANDLE_VALUE;
				return false;
			}
		}
		return true;
	}
} // namespace icemelt
