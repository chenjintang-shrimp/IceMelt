#include "raw/esp_raw.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>

#include "raw/block_io.h" // SectorIo
#include "raw/win_disk.h" // QueryVolumeInfo / VolumeInfo
#include "util.h"

namespace icemelt
{
	namespace
	{
		constexpr uint32_t kSector = 512;

		uint32_t RdU32(const uint8_t *p)
		{
			return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
			                             (static_cast<uint32_t>(p[3]) << 24));
		}

		uint64_t RdU64(const uint8_t *p)
		{
			uint64_t v = 0;
			for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
			return v;
		}

		// GPT 分区类型 GUID 的前 3 个字段（EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B）
		// 以磁盘上的小端布局比较：Data1 (LE)、Data2 (LE)、Data3 (LE)，其余 8 字节按字节序。
		const uint8_t kEspTypeGuid[16] = {
			0x28, 0x73, 0x2A, 0xC1, // C12A7328 (LE)
			0x1F, 0xF8,             // F81F (LE)
			0xD2, 0x11,             // 11D2 (LE)
			0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
	} // namespace

	// 一个磁盘的分区表（只读解析）。放在匿名命名空间之外：--selftest-partitions 要能
	// 拿合成镜像直接调它，把"保护性 MBR 判据写错字节"这类错误钉在自检里。
	bool ParseEspPartitions(
		SectorIo &                 io,
		uint32_t                   diskNumber,
		std::vector<EspPartition> &out,
		bool &                     gptFound,
		bool &                     mbrFound,
		std::vector<std::wstring> &notes,
		std::wstring &             error)
	{
		gptFound = false;
		mbrFound = false;
		uint8_t lba0[kSector] = {};
		if (!io.Read(0, lba0, kSector))
		{
			error = FormatW(L"disk %u: cannot read LBA0", diskNumber);
			return false;
		}
			// 保护性 MBR 的判据只看**类型字节 0xEE**，出现在 4 个条目中的任意一个。
			// 曾经多查了一个 CHS 字节（写成 [1]==0x02，而起始 CHS 是 [1]=0x00 [2]=0x02
			// [3]=0x00）—— 于是保护性 MBR 判不出来，掉进 MBR 分支找 0xEF，结论是
			// "没有任何磁盘上有 ESP"（VMware NVMe 上实测踩到）。
			bool protectiveMbr = false;
			for (int i = 0; i < 4; ++i)
			{
				if (lba0[0x1BE + i * 16 + 4] == 0xEE) protectiveMbr = true;
			}
			const bool mbrSignature = lba0[510] == 0x55 && lba0[511] == 0xAA;

			if (protectiveMbr)
			{
				gptFound = true;
				uint8_t header[kSector] = {};
				if (!io.Read(kSector, header, kSector))
				{
					error = FormatW(L"disk %u: cannot read the GPT header", diskNumber);
					return false;
				}
				if (std::memcmp(header, "EFI PART", 8) != 0)
				{
					error = FormatW(L"disk %u: protective MBR but no GPT header", diskNumber);
					return false;
				}
				const uint64_t entriesLba = RdU64(&header[0x48]);
				const uint32_t entryCount = RdU32(&header[0x50]);
				const uint32_t entrySize  = RdU32(&header[0x54]);
				if (entrySize < 128 || entrySize > 512 || entryCount == 0 || entryCount > 512)
				{
					error = FormatW(L"disk %u: implausible GPT entry geometry", diskNumber);
					return false;
				}
				const uint64_t tableBytes = static_cast<uint64_t>(entryCount) * entrySize;
				std::vector<uint8_t> table(static_cast<size_t>(tableBytes));
				if (!io.Read(entriesLba * kSector, table.data(), table.size()))
				{
					error = FormatW(L"disk %u: cannot read the GPT partition array", diskNumber);
					return false;
				}
				uint32_t usedEntries = 0;
				uint32_t espCount    = 0;
				for (uint32_t i = 0; i < entryCount; ++i)
				{
					const uint8_t *e = table.data() + static_cast<size_t>(i) * entrySize;
					bool           allZero = true;
					for (int b = 0; b < 16; ++b)
						if (e[b] != 0) allZero = false;
					if (allZero) continue;
					++usedEntries;
					if (std::memcmp(e, kEspTypeGuid, 16) != 0) continue;
					++espCount;

					EspPartition part;
					part.diskNumber  = diskNumber;
					part.offsetBytes = RdU64(&e[0x20]) * kSector;
					part.lengthBytes = (RdU64(&e[0x28]) - RdU64(&e[0x20]) + 1) * kSector;
					part.gpt         = true;
					part.description = FormatW(
						L"disk %u GPT partition %u (EFI System Partition filesystem)",
						diskNumber,
						i + 1);
					// 名字（UTF-16LE，最多 36 个字符）—— 有就带上，便于人识别
					std::wstring name;
					for (int c = 0; c < 36; ++c)
					{
						const wchar_t ch = static_cast<wchar_t>(
							e[0x38 + c * 2] | (e[0x38 + c * 2 + 1] << 8));
						if (ch == 0) break;
						name.push_back(ch);
					}
					if (!name.empty()) part.description += L" \"" + name + L"\"";
					out.push_back(part);
				}
				notes.push_back(
					FormatW(
						L"disk %u: GPT, %u partition(s), %u ESP match(es)",
						diskNumber,
						usedEntries,
						espCount));
				return true;
			}

			if (!mbrSignature)
			{
				// 没有分区表（软盘镜像、empty）—— 不是错误，只是没有 ESP
				return true;
			}
			// MBR：找类型 0xEF 的分区
			mbrFound = true;
			{
				std::wstring types;
				for (int i = 0; i < 4; ++i)
				{
					const uint8_t *e = lba0 + 0x1BE + i * 16;
					if (e[4] == 0x00 && e[12] == 0 && e[13] == 0 && e[14] == 0 && e[15] == 0)
						continue;
					types += FormatW(L" 0x%02X", e[4]);
				}
				notes.push_back(
					FormatW(L"disk %u: MBR, partition types:%ls", diskNumber, types.c_str()));
			}
			for (int i = 0; i < 4; ++i)
			{
				const uint8_t *e = lba0 + 0x1BE + i * 16;
				if (e[4] != 0xEF) continue;
				EspPartition part;
				part.diskNumber  = diskNumber;
				part.offsetBytes = static_cast<uint64_t>(RdU32(&e[8])) * kSector;
				part.lengthBytes = static_cast<uint64_t>(RdU32(&e[12])) * kSector;
				part.gpt         = false;
				part.description = FormatW(
					L"disk %u MBR partition %d (EFI System Partition, type 0xEF)",
					diskNumber,
					i + 1);
				out.push_back(part);
			}
			return true;
	}

	bool FindEspPartitions(
		uint32_t                    maxDisk,
		std::vector<EspPartition> & out,
		std::vector<std::wstring> & notes,
		std::wstring &              error)
	{
		out.clear();
		notes.clear();
		for (uint32_t disk = 0; disk <= maxDisk; ++disk)
		{
			PhysicalDriveSectorIo io;
			std::wstring          openError;
			if (!io.Open(disk, openError))
			{
				if (disk == 0)
				{
					error = openError;
					return false;
				}
				continue; // 不存在的盘号：正常
			}
			bool         gptFound  = false;
			bool         mbrFound  = false;
			std::wstring parseError;
			if (!ParseEspPartitions(
					io,
					disk,
					out,
					gptFound,
					mbrFound,
					notes,
					parseError))
			{
				// 某一号盘读不了分区表（离线、被别的程序独占…）：记下来但继续找
				notes.push_back(parseError);
				if (disk == 0 && out.empty()) error = parseError;
				io.Close();
				continue;
			}
			if (!gptFound && !mbrFound)
				notes.push_back(FormatW(L"disk %u: no partition table signature", disk));
			io.Close();
		}
		return true;
	}

	bool SelectEspPartition(
		const std::vector<EspPartition> &candidates,
		EspPartition &                   chosen,
		std::vector<std::wstring> &      notes,
		std::wstring &                   error)
	{
		if (candidates.empty())
		{
			error = L"no EFI System Partition found on any disk";
			return false;
		}
		if (candidates.size() == 1)
		{
			chosen = candidates.front();
			return true;
		}

		// 多个候选：优先选"装着本机系统卷"的那块盘上的（Windows 与它的 ESP 同盘是常态）
		VolumeInfo sysInfo;
		std::wstring sysError;
		if (QueryVolumeInfo(L"\\\\.\\C:", sysInfo, sysError))
		{
			int matches = 0;
			for (const auto &c: candidates)
			{
				if (c.diskNumber == sysInfo.diskNumber)
				{
					chosen = c;
					++matches;
				}
			}
			if (matches == 1)
			{
				notes.push_back(
					FormatW(
						L"several ESPs found; picked the one on disk %u (same disk as C:)",
						chosen.diskNumber));
				return true;
			}
		}
		for (const auto &c: candidates) notes.push_back(c.description);
		error = L"more than one EFI System Partition found: this build refuses to guess";
		return false;
	}
} // namespace icemelt
