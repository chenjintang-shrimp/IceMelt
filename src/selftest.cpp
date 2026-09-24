#include "selftest.h"

#include "raw/block_io.h"
#include "raw/fat_volume.h"
#include "raw/esp_raw.h"

#include <rpc.h> // UuidFromStringW：自检里用系统实现当 GUID 字节序的基准

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "melt/unfreeze.h"
#include "raw/ntfs_tool.h"
#include "raw/win_disk.h"
#include "reg/hive.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

namespace icemelt
{
	namespace
	{
		// 所有 CLI 输出都从这里走：cold & dark 着色（出错醒目红色、[!] 琥珀、其余白色）在同一处
		// 决定。重定向/不支持 VT 时 CliPaint 原样返回，日志里不会混入转义序列。
		void Print(const std::wstring &line)
		{
			std::printf("%s\n", Narrow(CliPaint(line)).c_str());
		}

		void Print(const char *line)
		{
			Print(Widen(line));
		}

		std::filesystem::path TempFile(const wchar_t *name)
		{
			std::vector<wchar_t> temp(MAX_PATH);
			const DWORD          need = ::GetTempPathW(
				static_cast<DWORD>(temp.size()),
				temp.data());
			const std::filesystem::path dir = (need > 0 && need < temp.size())
				                                  ? std::filesystem::path(temp.data())
				                                  : std::filesystem::path(
					                                  L"C:\\Windows\\Temp");
			return dir / name;
		}

		// --selftest-raw 用的图案：文件先写成 A，再经裸盘通路改成 B。
		constexpr unsigned char kPatternA = 0xA5;
		constexpr unsigned char kPatternB = 0x5A;

		// 一次读回的图案统计。把"读到了什么"讲成数字，而不是只报第一个不符的字节 ——
		// 判断"是旧内容还是别的"靠的就是这两个计数。
		struct Readback
		{
			size_t        bytesA              = 0;
			size_t        bytesB              = 0;
			size_t        other               = 0;
			size_t        firstDivergentFromB = 0;
			unsigned char divergentValue      = 0;
			bool          hasDivergence       = false;

			std::wstring Describe() const
			{
				std::wstring out = FormatW(
					L"0xA5 x%zu, 0x5A x%zu, other x%zu",
					bytesA,
					bytesB,
					other);
				if (hasDivergence)
				{
					out += FormatW(
						L"; first byte that is not 0x5A: #%zu = 0x%02X",
						firstDivergentFromB,
						divergentValue);
				}
				return out;
			}
		};

		Readback Summarize(const unsigned char *bytes, size_t size)
		{
			Readback out;
			for (size_t i = 0; i < size; ++i)
			{
				if (bytes[i] == kPatternA)
				{
					++out.bytesA;
				}
				else if (bytes[i] == kPatternB)
				{
					++out.bytesB;
				}
				else
				{
					++out.other;
				}
				if (!out.hasDivergence && bytes[i] != kPatternB)
				{
					out.hasDivergence       = true;
					out.firstDivergentFromB = i;
					out.divergentValue      = bytes[i];
				}
			}
			return out;
		}

		// 按区段拼读一段物理区域（底层通路：驱动直达磁盘）。区段总长不足时返回 false ——
		// 那说明 FSCTL 给出的映射覆盖不了这个文件，后面的判据都不成立。
		bool ReadExtentsRaw(
			WinDiskDevice &             device,
			const std::vector<Extent> & extents,
			size_t                      want,
			std::vector<unsigned char> &out,
			std::wstring &              error)
		{
			out.assign(want, 0);
			size_t read = 0;
			for (const auto &extent: extents)
			{
				if (read >= want) break;
				const size_t chunk = static_cast<size_t>((extent.length < want - read)
					                                         ? extent.length
					                                         : want - read);
				if (chunk == 0) continue;
				if (!device.ReadAt(
					extent.physicalOffset,
					out.data() + read,
					chunk,
					error)) return false;
				read += chunk;
			}
			if (read < want)
			{
				error = FormatW(L"the extents cover only %zu of %zu bytes", read, want);
				return false;
			}
			return true;
		}
	} // namespace

	int HiveSelfTest(const std::filesystem::path &exeDir)
	{
		(void) exeDir;
		const std::filesystem::path hive = TempFile(L"icemelt-selftest.hive");
		Print(L"IceMelt hive self-test");
		Print(FormatW(L"target: %ls", hive.c_str()));

		std::wstring error;
		if (!ExportSystemHive(hive, error))
		{
			Print(FormatW(L"FAIL export: %ls", error.c_str()));
			return 1;
		}
		Print(L"exported SYSTEM hive (RegSaveKeyEx, REG_LATEST_FORMAT)");

		HiveBaseBlock before;
		if (!ReadBaseBlock(hive, before, error))
		{
			Print(FormatW(L"FAIL read base block: %ls", error.c_str()));
			return 1;
		}
		Print(L"--- base block as saved ---");
		Print(
			FormatW(
				L"signature        : %ls",
				before.signatureOk ? L"regf" : L"INVALID"));
		Print(FormatW(L"sequence1        : %u", before.sequence1));
		Print(FormatW(L"sequence2        : %u", before.sequence2));
		Print(FormatW(L"major.minor      : %u.%u", before.major, before.minor));
		Print(FormatW(L"type             : %u (0 = primary hive)", before.type));
		Print(FormatW(L"format           : %u", before.format));
		Print(FormatW(L"root cell offset : %u", before.rootCell));
		Print(FormatW(L"hive bins size   : %u", before.binsSize));
		Print(FormatW(L"clustering factor: %u", before.clusteringFactor));
		Print(FormatW(L"stored checksum  : 0x%08X", before.storedChecksum));
		Print(FormatW(L"computed checksum: 0x%08X", before.computedChecksum));
		Print(FormatW(L"file size        : %llu", before.fileSize));
		Print(
			FormatW(
				L"bins == size-4K  : %ls",
				(before.binsSize + kHiveBlockSize == before.fileSize) ? L"yes" : L"no"));

		HiveBaseBlock after;
		if (!MakeCleanAndFixChecksum(hive, after, error))
		{
			Print(FormatW(L"FAIL clean/fix: %ls", error.c_str()));
			return 1;
		}
		Print(
			FormatW(
				L"base block rewritten=%ls",
				(before.sequence2 != after.sequence2 || before.storedChecksum != after.
					storedChecksum)
					? L"yes"
					: L"no (already clean and valid)"));

		bool       ok    = true;
		const auto check = [&](const wchar_t *name, bool value)
		{
			Print(FormatW(L"%-34ls: %ls", name, value ? L"OK" : L"FAILED"));
			if (!value) ok = false;
		};
		Print(L"--- assertions ---");
		check(L"signature is \"regf\"", after.signatureOk);
		check(L"computedChecksum == storedChecksum", after.checksumOk);
		check(L"sequence1 == sequence2", after.clean);
		check(L"fileSize % 4096 == 0", after.fileSize % kHiveBlockSize == 0);
		check(
			L"root cell is an \"nk\" cell",
			[&]
			{
				std::wstring verifyError;
				// VerifyHiveLoadable 覆盖同一批断言，这里直接复用它来判定 root cell
				return VerifyHiveLoadable(hive, verifyError);
			}());
		if (!ok) Print(L"FAILED: regf layout constants or checksum algorithm are wrong");
		Print(ok ? L"PASS" : L"FAIL");
		return ok ? 0 : 1;
	}

	int RegistrySelfTest(const std::filesystem::path &exeDir)
	{
		// scratch 键落在 HKLM\SOFTWARE 下：结构与设备类键相同（同一段代码处理），
		// 但绝不涉及任何设备类，因此不可能影响下次启动的设备/驱动加载。
		constexpr const wchar_t *kScratchRoot = L"SOFTWARE\\IceMelt\\Selftest";
		constexpr const wchar_t *kFakeGuid = L"{00000000-0000-0000-0000-0000000000FF}";
		const std::wstring       classPath = std::wstring(kScratchRoot) + L"\\Class\\" +
				kFakeGuid;

		Print(L"IceMelt registry self-test");
		Print(
			FormatW(
				L"scratch key: HKLM\\%ls (no device class key is touched)",
				classPath.c_str()));

		::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kScratchRoot); // 从干净状态开始

		HKEY    key    = nullptr;
		LSTATUS status = ::RegCreateKeyExW(
			HKEY_LOCAL_MACHINE,
			classPath.c_str(),
			0,
			nullptr,
			0,
			KEY_ALL_ACCESS,
			nullptr,
			&key,
			nullptr);
		if (status != ERROR_SUCCESS)
		{
			Print(
				FormatW(
					L"FAIL create scratch key: %ld (registry self-test needs elevation)",
					status));
			return 1;
		}

		const auto setValue = [&](
			const wchar_t *                  name,
			const std::vector<std::wstring> &items)
		{
			std::vector<wchar_t> buffer;
			for (const auto &item: items)
			{
				buffer.insert(buffer.end(), item.begin(), item.end());
				buffer.push_back(L'\0');
			}
			buffer.push_back(L'\0');
			return ::RegSetValueExW(
						key,
						name,
						0,
						REG_MULTI_SZ,
						reinterpret_cast<const BYTE *>(buffer.data()),
						static_cast<DWORD>(buffer.size() * sizeof(wchar_t))) ==
					ERROR_SUCCESS;
		};
		const auto getValue = [&](const wchar_t *name, std::vector<std::wstring> &items)
		{
			items.clear();
			DWORD type  = 0;
			DWORD bytes = 0;
			if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
				ERROR_SUCCESS)
				return false;
			std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
			if (::RegQueryValueExW(
				key,
				name,
				nullptr,
				&type,
				reinterpret_cast<LPBYTE>(buffer.data()),
				&bytes) != ERROR_SUCCESS)
				return false;
			const wchar_t *p   = buffer.data();
			const wchar_t *end = buffer.data() + bytes / sizeof(wchar_t);
			while (p < end && *p != L'\0')
			{
				items.emplace_back(p);
				p += items.back().size() + 1;
			}
			return true;
		};

		// 名单 = 真实名单 + IceMeltProbe；插入的项里故意含一个大小写变体（DEEPFRZ）来验证不区分大小写
		auto names = TargetNames(LoadTargets(exeDir));
		names.emplace_back(L"IceMeltProbe");

		bool       ok    = true;
		const auto check = [&](const wchar_t *what, bool value)
		{
			Print(FormatW(L"%-46ls: %ls", what, value ? L"OK" : L"FAILED"));
			if (!value) ok = false;
		};

		// 用例 1：列表里混有系统自身的项 + 命中项 → 只删命中项，其余原样保留且顺序不变
		setValue(
			L"UpperFilters",
			{L"partmgr", L"IceMeltProbe", L"DEEPFRZ", L"OtherFilter"});
		EditReport report = StripFilterValueIn(HKEY_LOCAL_MACHINE, classPath, kFakeGuid, names);
		std::vector<std::wstring> items;
		const bool present = getValue(L"UpperFilters", items);
		Print(FormatW(L"UpperFilters now: %zu item(s)", items.size()));
		for (const auto &item: items) Print(L"  " + item);
		check(
			L"matched entries removed, others kept",
			present && items.size() == 2 &&
			items[0] == L"partmgr" &&
			items[1] == L"OtherFilter");
		check(L"report lists the changed value", report.changedValues.size() == 1);
		check(L"no write failures reported", report.failures.empty());

		// 用例 2：列表里全是命中项 → 整值删除（而不是留下空数组）
		setValue(L"LowerFilters", {L"IceMeltProbe", L"deEPfrZ"});
		const EditReport emptyCase = StripFilterValueIn(HKEY_LOCAL_MACHINE, classPath, kFakeGuid, names);
		std::vector<std::wstring> lower;
		check(
			L"all-matched value is deleted entirely",
			!getValue(L"LowerFilters", lower));
		check(L"deletion is reported as a change", emptyCase.changedValues.size() == 1);

		// 用例 2b：值是单项 REG_SZ（部分安装程序这么写）→ 必须能读出来、能删掉、类型不被改写成 MULTI_SZ
		{
			const wchar_t *probe = L"IceMeltProbe";
			const DWORD bytes = static_cast<DWORD>((wcslen(probe) + 1) * sizeof(wchar_t));
			const bool wrote = ::RegSetValueExW(
						key,
						L"LowerFilters",
						0,
						REG_SZ,
						reinterpret_cast<const BYTE *>(probe),
						bytes) ==
					ERROR_SUCCESS;
			const EditReport szCase = StripFilterValueIn(HKEY_LOCAL_MACHINE, classPath, kFakeGuid, names);
			DWORD            type   = 0;
			DWORD            size   = 0;
			const bool       gone   = ::RegQueryValueExW(
						key,
						L"LowerFilters",
						nullptr,
						&type,
						nullptr,
						&size) ==
					ERROR_FILE_NOT_FOUND;
			check(
				L"REG_SZ single item is matched and removed (not skipped)",
				wrote && szCase.changedValues.size() == 1 && szCase.failures.empty() &&
				gone);
		}

		// 用例 2c：REG_SZ 列表里含未命中项 → 写回时必须仍是 REG_SZ（不能悄悄升级成 MULTI_SZ）
		{
			const wchar_t *value = L"partmgr";
			const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
			::RegSetValueExW(
				key,
				L"LowerFilters",
				0,
				REG_SZ,
				reinterpret_cast<const BYTE *>(value),
				bytes);
			const EditReport szKept = StripFilterValueIn(HKEY_LOCAL_MACHINE, classPath, kFakeGuid, names);
			check(
				L"REG_SZ without a match stays untouched",
				szKept.changedValues.empty());
		}

		// 用例 3：列表里没有命中项 → 一个字节都不动
		setValue(L"UpperFilters", {L"partmgr", L"OtherFilter"});
		const EditReport untouched = StripFilterValueIn(
			HKEY_LOCAL_MACHINE,
			classPath,
			kFakeGuid,
			{L"SomethingElse"});
		check(L"no match means no change reported", untouched.changedValues.empty());

		::RegCloseKey(key);
		::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kScratchRoot);
		HKEY       leftover = nullptr;
		const bool removed  =
				::RegOpenKeyExW(
					HKEY_LOCAL_MACHINE,
					kScratchRoot,
					0,
					KEY_READ,
					&leftover) != ERROR_SUCCESS;
		if (!removed) ::RegCloseKey(leftover);
		check(L"scratch key cleaned up", removed);

		Print(ok ? L"PASS" : L"FAIL");
		return ok ? 0 : 1;
	}

	int RawSelfTest(const std::filesystem::path &exeDir)
	{
		Print(L"IceMelt raw disk self-test");
		Print(
			L"WARNING: this loads a kernel driver and writes to the raw disk of the system volume.");

		const std::filesystem::path scratch      = L"C:\\icemelt-selftest.bin";
		constexpr size_t            kScratchSize = 64 * 1024;

		std::wstring                error;
		const std::filesystem::path sysPath = exeDir / L"WinDisk_x64.sys";
		const DriverLoad            load    = LoadDriver(sysPath, L"WinDisk", error);
		if (load != DriverLoad::Loaded)
		{
			Print(FormatW(L"FAIL load driver: %ls", error.c_str()));
			if (load == DriverLoad::SignatureRejected)
			{
				Print(
					L"     the load was rejected with 577 (ERROR_INVALID_IMAGE_HASH): run "
					L"'kdu -dse 0' first, then retry -- a positive DSE state read out of "
					L"NtQuerySystemInformation does not mean the load will be allowed");
			}
			return 1;
		}
		Print(L"driver loaded");

		WinDiskDevice device;
		if (!device.Open(error))
		{
			Print(FormatW(L"FAIL open device: %ls", error.c_str()));
			return 1;
		}

		VolumeInfo vol;
		if (!QueryVolumeInfo(L"\\\\.\\C:", vol, error))
		{
			Print(FormatW(L"FAIL volume info: %ls", error.c_str()));
			return 1;
		}
		Print(
			FormatW(
				L"volume: disk=%u offset=%llu length=%llu bps=%u spc=%u",
				vol.diskNumber,
				vol.volumeOffset,
				vol.volumeLength,
				vol.bytesPerSector,
				vol.sectorsPerCluster));

		if (!device.SetTargetDisk(vol.diskNumber, error))
		{
			Print(FormatW(L"FAIL CTL_CHANGE_TARGET_DISK: %ls", error.c_str()));
			return 1;
		}

		// 用普通 API 造一个已知内容的文件，再通过驱动按物理区段改写、用普通 API 读回
		bool ok = true;
		{
			HANDLE file = ::CreateFileW(
				scratch.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				0,
				nullptr,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				Print(FormatW(L"FAIL create %ls: %u", scratch.c_str(), ::GetLastError()));
				return 1;
			}
			std::vector<unsigned char> buffer(kScratchSize, kPatternA);
			DWORD                      written = 0;
			ok                                 = ::WriteFile(
						file,
						buffer.data(),
						static_cast<DWORD>(buffer.size()),
						&written,
						nullptr) &&
					written == buffer.size();
			// 把 A 真正刷到盘上再关句柄：这样文件系统缓存是"干净的"，后面裸写进去的 B
			// 不会再被缓存里的脏页覆盖，读回结果才有意义。
			if (ok) ok = ::FlushFileBuffers(file) != FALSE;
			::CloseHandle(file);
			if (!ok)
			{
				Print(L"FAIL write pattern A through the normal API");
				return 1;
			}
		}
		Print(L"scratch file created with pattern 0xA5 (flushed to disk)");

		std::vector<Extent> extents;
		if (!ListExtentsPhysical(scratch, vol, extents, error))
		{
			Print(FormatW(L"FAIL enumerate extents: %ls", error.c_str()));
			::DeleteFileW(scratch.c_str());
			return 1;
		}
		Print(FormatW(L"physical extents: %zu", extents.size()));
		for (size_t i = 0; i < extents.size(); ++i)
		{
			Print(
				FormatW(
					L"  #%zu offset=%llu length=%llu",
					i + 1,
					extents[i].physicalOffset,
					extents[i].length));
		}
		ok = !extents.empty();
		if (!ok) Print(L"FAIL no extents");

		// 读回分两条路，判据完全不同 —— 报告里必须讲清每条走的是哪条路：
		//   (1) 底层读回：经 WinDisk 驱动直达磁盘 miniport。它证明的是"写确实落到了这些
		//       物理偏移上"，与文件系统看到的无关。
		//   (2) OS 读回：经文件系统（FILE_FLAG_NO_BUFFERING 绕开缓存，但**不绕过滤器**）。
		//       它证明的是"这台机器上文件系统看到的文件内容"。
		// 冻结状态（冰点/影子系统这类卷过滤器）下两者**必然**不一致：文件系统的写被过滤器
		// 留在上层，驱动却能直接改磁盘，于是底层读到 B、OS 侧仍读到 A。这不是失败，恰恰是
		// 过滤器在工作 —— 所以 (1) 是硬断言，(2) 只做分类报告（见下面 classify）。
		Readback preRead;
		if (ok)
		{
			// 先读一次基准：这些区段在裸写之前是什么。它决定"OS 侧读到 A"该如何解读 ——
			// 若基准不是 A，说明文件系统的写根本没到磁盘（被上层过滤器截住了），
			// 那么 OS 侧读回 A 就完全在预期之内。
			std::vector<unsigned char> before;
			if (!ReadExtentsRaw(device, extents, kScratchSize, before, error))
			{
				Print(FormatW(L"FAIL raw pre-read: %ls", error.c_str()));
				ok = false;
			}
			else
			{
				preRead = Summarize(before.data(), before.size());
				Print(
					FormatW(
						L"raw pre-read (before any raw write): %ls",
						preRead.Describe().c_str()));
				if (preRead.bytesA == before.size())
				{
					Print(
						L"  -> the file-system write reached the disk; the extent mapping is exact");
				}
				else if (preRead.bytesB == before.size())
				{
					Print(
						L"  -> these extents still hold 0x5A, i.e. what a previous run of this "
						L"self-test wrote over the raw disk; the file's own content (0xA5) never "
						L"reached them");
				}
				else
				{
					Print(
						L"  -> the file-system write did NOT reach the disk (a volume filter above "
						L"the FS is holding it; a freeze/restore product does exactly this)");
				}
			}
		}

		std::vector<unsigned char> patternB(kScratchSize, kPatternB);
		size_t                     offset = 0;
		for (const auto &extent: extents)
		{
			if (!ok) break;
			if (offset >= patternB.size()) break;
			const size_t chunk = static_cast<size_t>(
				(extent.length < patternB.size() - offset)
					? extent.length
					: patternB.size() - offset);
			if (!device.WriteAt(
				extent.physicalOffset,
				patternB.data() + offset,
				chunk,
				error))
			{
				Print(FormatW(L"FAIL raw write: %ls", error.c_str()));
				ok = false;
				break;
			}
			offset += chunk;
		}
		if (ok && offset < patternB.size())
		{
			Print(
				FormatW(
					L"FAIL extents cover only %zu of %zu bytes",
					offset,
					patternB.size()));
			ok = false;
		}

		if (ok)
		{
			// (1) 底层读回：必须读到 B —— 这是"驱动按这些物理偏移写进去了"的唯一判据。
			// （普通 ReadFile 会命中 FS 缓存里的旧页，看出"写了没生效"，其实写对了。）
			std::vector<unsigned char> rawRead;
			if (!ReadExtentsRaw(device, extents, kScratchSize, rawRead, error))
			{
				Print(FormatW(L"FAIL raw read-back: %ls", error.c_str()));
				ok = false;
			}
			else
			{
				const Readback raw = Summarize(rawRead.data(), rawRead.size());
				if (raw.bytesB == rawRead.size())
				{
					Print(
						L"raw read-back (driver path) matches pattern 0x5A: the write landed on disk");
				}
				else
				{
					Print(
						FormatW(
							L"FAIL raw read-back (driver path) does not match pattern 0x5A: %ls",
							raw.Describe().c_str()));
					Print(
						L"     the driver path itself is broken (target disk, offsets or the write "
						L"path) - this is a real failure, not a filter");
					ok = false;
				}
			}
		}

		if (ok)
		{
			// (2) OS 读回，非硬断言：见本节开头的两条通路说明。FILE_FLAG_NO_BUFFERING 要求
			// 缓冲区按扇区对齐（VirtualAlloc 给的是 64 KiB 对齐）且长度是扇区整数倍（64 KiB ✓）。
			const DWORD sector  = vol.bytesPerSector ? vol.bytesPerSector : 512;
			void *      aligned = ::VirtualAlloc(
				nullptr,
				kScratchSize,
				MEM_COMMIT | MEM_RESERVE,
				PAGE_READWRITE);
			HANDLE file = ::CreateFileW(
				scratch.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_NO_BUFFERING,
				nullptr);
			if (!aligned || file == INVALID_HANDLE_VALUE)
			{
				Print(FormatW(L"FAIL open scratch uncached: %u", ::GetLastError()));
				ok = false;
			}
			else
			{
				DWORD      got    = 0;
				const bool readOk =
						::ReadFile(
							file,
							aligned,
							static_cast<DWORD>(kScratchSize),
							&got,
							nullptr) &&
						got == kScratchSize;
				::CloseHandle(file);
				if (!readOk)
				{
					Print(
						FormatW(
							L"FAIL uncached read (sector=%u): %u",
							sector,
							::GetLastError()));
					ok = false;
				}
				else
				{
					const auto *   bytes = static_cast<const unsigned char *>(aligned);
					const Readback os    = Summarize(bytes, kScratchSize);
					Print(
						FormatW(
							L"OS read-back (file-system path, uncached): %ls",
							os.Describe().c_str()));
					if (os.bytesB == kScratchSize)
					{
						Print(
							L"  -> the file system sees what the driver wrote: nothing between the FS "
							L"and the disk is shadowing this file");
					}
					else if (os.bytesA == kScratchSize)
					{
						Print(
							L"  -> the file system still sees 0xA5: a volume filter is holding the "
							L"original blocks. Expected on a frozen machine - the raw write below it "
							L"succeeded anyway, which is exactly what melt relies on.");
					}
					else
					{
						Print(
							L"FAIL the OS read-back is a mix (see the counts above), not a clean "
							L"all-A or all-B result: the file's extents are shared with other data, "
							L"or something modified the file while the test ran");
						ok = false;
					}
				}
			}
			if (aligned) ::VirtualFree(aligned, 0, MEM_RELEASE);
		}

		::DeleteFileW(scratch.c_str());
		Print(ok ? L"PASS" : L"FAIL");
		return ok ? 0 : 1;
	}

	int MeltApply(const std::filesystem::path &exeDir)
	{
		// 非交互落盘。确认契约在这里的体现：调用方必须先给出 --yes-i-know，
		// 否则 main() 根本不会走到这里；因此这个回调恒为 true 是"用户已确认"的直接后果，
		// 而不是把确认步骤删掉（图形前端那条路仍然弹框）。
		MeltOptions opt;
		opt.dryRun         = false;
		opt.exeDir         = exeDir;
		opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";
		// 这一版不复位：写完并逐字节验证之后交回给操作者，由他自己重启（理由见 RunMelt 第 10 步）。
		opt.confirm        = [](const MeltResult &)
		{
			return true;
		};

		Print(L"IceMelt melt (non-interactive; --yes-i-know was given)");
		Print(
			L"WARNING: this overwrites the SYSTEM hive on the raw disk and also its RegBack copy,");
		Print(
			L"         so no earlier hive generation survives on this machine. There is no rollback:");
		Print(
			L"         System Restore cannot undo it - only a VM snapshot taken beforehand can.");
		Print(
			L"No reset is triggered: once the writes verify, reboot the machine yourself with a normal");
		Print(
			L"restart - that is what applies the change. The log stays on screen until you do.");
		const MeltResult result = RunMelt(
			opt,
			[](const std::wstring &line)
			{
				Print(line);
			});
		if (!result.pending.empty())
		{
			Print(L"--- planned actions ---");
			for (const auto &item: result.pending) Print(L"  " + item);
		}
		if (!result.ok)
		{
			// ok=true 只有两种来路：dryRun 预演跑完，或落盘路径写完且逐字节验证通过。
			// 所以"走到这里且 ok=false"就是中途失败（日志里已有原因）。
			Print(
				result.wroteDisk
					? L"FAILED: the raw disk was already written - state may be inconsistent, "
					L"reboot now or restore from backup"
					: L"FAILED: nothing was written to the raw disk");
			return 1;
		}
		Print(L"PASS");
		return 0;
	}

	int MeltDryRun(const std::filesystem::path &exeDir)
	{
		MeltOptions opt;
		opt.dryRun         = true;
		opt.exeDir         = exeDir;
		opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";

		MeltResult result = DryRun(opt, nullptr);
		for (const auto &line: result.log) Print(line);
		Print(L"--- planned actions (no raw disk write happened) ---");
		for (const auto &item: result.pending) Print(L"  " + item);
		Print(result.ok ? L"PASS" : L"FAIL");
		return result.ok ? 0 : 1;
	}

	int PreflightScan(const std::filesystem::path &exeDir)
	{
		// --preflight: melt 的自动位置扫描入口。参数形状与 --melt 相同：不做注册表
		// 编辑/hive 写回，不触发复位；唯一共享的粘合是：
		//   A) 驱动装载（可能 kdu -dse 0）
		//   B) 打开 handle: 设备做 raw I/O。
		// 输出与 melt 相同的行级日志，方便与 melt 同一番话术。
		MeltOptions opt;
		opt.dryRun         = false;
		opt.exeDir         = exeDir;
		opt.winDiskSysPath = exeDir / L"WinDisk_x64.sys";

		Print(L"IceMelt preflight scan (non-interactive)");
		Print(
			L"WARNING: this does raw disk I/O to the same system volume a melt would use;");
		Print(
			L"         it does NOT write SYSTEM/RegBack or reset, but it does load an unsigned");
		Print(L"         driver. Only meaningful on a VM with a snapshot.");
		const MeltResult result = RunPreflightScan(
			opt,
			[](const std::wstring &line)
			{
				Print(line);
			});
		Print(result.ok ? L"PREFLIGHT PASS" : L"PREFLIGHT FAIL (see scan lines above)");
		return result.ok ? 0 : 1;
	}
	// ---------------------------------------------------------------- FAT 自检
	//
	// 这一项存在的理由：ESP 上要写的是 FAT，而写路径是"裸扇区"（没有文件系统替我们兜底）。
	// 一个错的 FAT 写路径会把 ESP 写坏 —— 那台机器就再也引导不进 Windows。所以：
	//   * 全部动作在一个**合成镜像**上做（%TEMP% 的普通文件），不碰任何真实卷；
	//   * 图像由 f_mkfs（FormatFat32ForTest）格式化，自检不依赖管理员/驱动/真磁盘；
	//   * 交叉验证交给外部独立实现：打印镜像路径，用 pyfatfs 读它；
	//   * 反向（我们的读 + LFN 查找）用 --image=<外部镜像> 校验 —— 真实 ESP 的
	//     \EFI\Microsoft 是 LFN 名字，这条必须靠外部造的镜像验；
	//   * I/O 计数把"每个簇一次系统调用"这类退化钉死（曾经把磁盘打到 85%）。
	int FatSelfTest(const std::filesystem::path &exeDir, const std::wstring &imagePath)
	{
		(void) exeDir;
		bool ok = true;
		const auto check = [&](const wchar_t *what, bool value)
		{
			Print(FormatW(L"%-54ls: %ls", what, value ? L"OK" : L"FAILED"));
			if (!value) ok = false;
		};

		// 与写入器同源的图案生成：外部工具（pyfatfs）也能按同一公式造/验内容
		const auto pattern = [](unsigned char seed)
		{
			return [seed](uint64_t offset, void *dst, size_t len) -> bool
			{
				auto *p = static_cast<unsigned char *>(dst);
				for (size_t i = 0; i < len; ++i)
					p[i] = static_cast<unsigned char>(
						(seed + ((offset + i) * 31 + 17)) & 0xFF);
				return true;
			};
		};

		if (!imagePath.empty())
		{
			// ---- 只读校验一个外部造的镜像（验证 LFN 查找 + 内容读取）----
			Print(FormatW(L"--- FAT read-only check on %ls ---", imagePath.c_str()));
			FileSectorIo io;
			std::wstring error;
			if (!io.Open(imagePath, /*create=*/false, 0, error))
			{
				Print(FormatW(L"FAIL open: %ls", error.c_str()));
				return 1;
			}
			FatFsVolume fs;
			if (!fs.Mount(io, 0, error))
			{
				Print(FormatW(L"FAIL mount: %ls", error.c_str()));
				io.Close();
				return 1;
			}
			Print(FormatW(L"type=%ls cluster=%llu bytes free=%llu",
			              fs.type(),
			              static_cast<unsigned long long>(fs.clusterSize()),
			              static_cast<unsigned long long>(fs.freeBytes())));

			// 外部镜像里应有的结构：LFN 目录（Microsoft / Boot）+ 一个按图案写的文件
			const char *kPaths[] = {
				"EFI\\Microsoft\\Boot\\BOOTMGFW.EFI",
				"EFI\\Boot\\BOOTX64.EFI",
				"EFI\\IceMelt\\SYSTEM",
				"EFI\\IceMelt\\PATTERN.BIN",
			};
			for (const char *path: kPaths)
			{
				uint64_t size  = 0;
				bool     isDir = false;
				const bool exists = fs.Exists(path, &size, &isDir, error);
				check(Widen(path).c_str(), exists);
				if (exists)
					Print(FormatW(
						L"    %hs: %llu bytes%s",
						path,
						static_cast<unsigned long long>(size),
						isDir ? L" (dir)" : L""));
			}

			// 内容校验：PATTERN.BIN 必须是 (seed + (off*31+17)) 的重复图案
			uint64_t patternSize = 0;
			if (fs.Exists("EFI\\IceMelt\\PATTERN.BIN", &patternSize, nullptr, error))
			{
				std::vector<uint8_t> bytes;
				if (!fs.ReadFile("EFI\\IceMelt\\PATTERN.BIN", bytes, error))
				{
					Print(FormatW(L"FAIL read PATTERN.BIN: %ls", error.c_str()));
					ok = false;
				}
				else
				{
					std::vector<uint8_t> expected(bytes.size());
					pattern(0x5A)(0, expected.data(), expected.size());
					check(
						L"PATTERN.BIN content matches the shared formula",
						std::memcmp(bytes.data(), expected.data(), bytes.size()) == 0);
				}
			}
			io.Close();
			Print(ok ? L"FAT SELFTEST PASS" : L"FAT SELFTEST FAIL");
			return ok ? 0 : 1;
		}

		// ---- 在合成镜像上跑写入器 ----
		// 几何要落在规范内：FAT32 要求簇数 >= 65525（[MS-FAT] / pyfatfs 都按这条判型）。
		// 64 MB + 512 字节簇 → ~129k 簇，正好和真实的 100 MB ESP 同型（Windows 给小的
		// FAT32 卷就是 1 扇区/簇）。用 48 MB + 4 KiB 簇会造出一个"技术上不是 FAT32"的卷，
		// 外部实现拒绝对它判型 —— 那是自检镜像的问题，不是写入器的问题。
		const uint64_t kImageSize = 64ull * 1024 * 1024;
		const std::filesystem::path image = TempFile(L"icemelt-fat-selftest.img");
		Print(L"--- FAT write path self-test on a synthetic image (FatFs R0.15) ---");
		Print(FormatW(L"image: %ls (%llu MB)", image.c_str(), kImageSize / (1024 * 1024)));

		FileSectorIo io;
		std::wstring error;
		if (!io.Open(image, /*create=*/true, kImageSize, error))
		{
			Print(FormatW(L"FAIL create image: %ls", error.c_str()));
			return 1;
		}
		if (!FormatFat32ForTest(io, 0, kImageSize, 512, /*sectorsPerCluster=*/1, error))
		{
			Print(FormatW(L"FAIL format: %ls", error.c_str()));
			io.Close();
			return 1;
		}
		FatFsVolume fs;
		if (!fs.Mount(io, 0, error))
		{
			Print(FormatW(L"FAIL mount: %ls", error.c_str()));
			io.Close();
			return 1;
		}
		check(L"mount", fs.mounted());
		check(L"recognised as FAT32", std::wcscmp(fs.type(), L"FAT32") == 0);
		check(L"cluster size is 512 B (1 sector, like a small ESP)", fs.clusterSize() == 512);

		// 目录结构与真实 ESP 同形：\EFI\Microsoft\Boot 里的 "Microsoft" 只能用 LFN
		// 表达（8.3 装不下），所以这条同时验证 LFN 条目生成与 LFN 查找。
		check(L"mkdir EFI", fs.MakeDirectory("EFI", error));
		check(L"mkdir EFI\\Microsoft (long name via LFN)", fs.MakeDirectory("EFI\\Microsoft", error));
		check(L"mkdir EFI\\Microsoft\\Boot", fs.MakeDirectory("EFI\\Microsoft\\Boot", error));
		check(L"mkdir EFI\\Boot", fs.MakeDirectory("EFI\\Boot", error));
		check(L"mkdir EFI\\IceMelt (idempotent)", fs.MakeDirectory("EFI\\IceMelt", error));
		check(
			L"the long name resolves (LFN lookup)",
			fs.Exists("EFI\\Microsoft\\Boot", nullptr, nullptr, error));

		const uint64_t big     = 3ull * 1024 * 1024 + 12345;
		const uint64_t smaller = 900ull * 1024 + 7;
		const uint64_t payload = 12ull * 1024 * 1024 + 4711;

		check(
			L"write bootmgfw under the LFN path",
			fs.WriteFile("EFI\\Microsoft\\Boot\\BOOTMGFW.EFI", 1536 * 1024, pattern(0x11), error));
		check(
			L"verify it",
			fs.VerifyFile("EFI\\Microsoft\\Boot\\BOOTMGFW.EFI", 1536 * 1024, pattern(0x11), error));
		check(L"write a 3 MB file", fs.WriteFile("EFI\\Boot\\BOOTX64.EFI", big, pattern(0x44), error));
		check(L"verify it", fs.VerifyFile("EFI\\Boot\\BOOTX64.EFI", big, pattern(0x44), error));
		check(
			L"replace it with a smaller file",
			fs.WriteFile("EFI\\Boot\\BOOTX64.EFI", smaller, pattern(0x22), error));
		check(
			L"verify after replace",
			fs.VerifyFile("EFI\\Boot\\BOOTX64.EFI", smaller, pattern(0x22), error));
		{
			uint64_t size  = 0;
			bool     isDir = true;
			const bool got = fs.Exists("EFI\\Boot\\BOOTX64.EFI", &size, &isDir, error);
			check(L"Exists reports the new size", got && size == smaller && !isDir);
		}
		// 目录树必须真的是树：根目录里不能看见孙辈。这条断言存在的理由是一次真实事故 ——
		// 测试格式化器把根簇的 EOC 写错了偏移，分配器于是把子目录分到了根簇上；读写器
		// 自洽，所有"能读到自己写的东西"的断言全绿，只有独立实现（pyfatfs）看出来塌了。
		check(
			L"a subdirectory's contents are not reachable from the root",
			!fs.Exists("BOOTX64.EFI", nullptr, nullptr, error) &&
			!fs.Exists("Boot\\BOOTX64.EFI", nullptr, nullptr, error));
		check(
			L"the root does not contain a nested EFI directory",
			!fs.Exists("EFI\\EFI", nullptr, nullptr, error));

		// 性能回归：一次 12 MB 的写入 + 读回校验，diskio 层的调用次数必须**与簇数同阶**
		// （FatFs 每次下发尽量多个连续扇区，但一批不会超过一个簇）。手写的那版在这里是
		// 六位数：FAT 表项逐条读、逐条写（每簇 ×2 份 FAT ×读写）、freeBytes 再逐簇扫一遍 ——
		// 现场表现就是磁盘 85% 且长时间没有任何输出。
		// 判据取几何界：簇数 + 25% + 1000 的余量。它同时说明了"为什么这个数字是合理的"，
		// 而不是拍一个常数（换簇大小/换文件大小都不会误报）。
		EnableFatIoCounters(true);
		ResetFatIoCounters();
		check(
			L"write a 12 MB payload",
			fs.WriteFile("EFI\\IceMelt\\SYSTEM", payload, pattern(0x33), error));
		check(
			L"verify the payload",
			fs.VerifyFile("EFI\\IceMelt\\SYSTEM", payload, pattern(0x33), error));
		const FatIoStats payloadIo    = FatIoCounters();
		EnableFatIoCounters(false);
		const uint64_t   clustersUsed = payload / fs.clusterSize();
		const uint64_t   ioBound      = clustersUsed + clustersUsed / 4 + 1000;
		Print(FormatW(
			L"    payload I/O: %llu disk writes / %llu disk reads for %llu clusters "
			L"(%.0f bytes per call)",
			static_cast<unsigned long long>(payloadIo.writes),
			static_cast<unsigned long long>(payloadIo.reads),
			static_cast<unsigned long long>(clustersUsed),
			static_cast<double>(payload) /
					static_cast<double>(payloadIo.writes + payloadIo.reads)));
		check(
			FormatW(
				L"12 MB write+verify stays at ~1 disk call per cluster (<= %llu)",
				static_cast<unsigned long long>(ioBound))
				.c_str(),
			payloadIo.writes <= ioBound && payloadIo.reads <= ioBound);

		// 空闲空间统计也不能退化成逐簇扫描：一次 fresh 的 f_getfree 在 FAT 里扫，
		// 但按扇区走（64 MB 镜像的 FAT 只有 512 KB）
		{
			EnableFatIoCounters(true);
			ResetFatIoCounters();
			const uint64_t freeNow = fs.freeBytes();
			const FatIoStats freeIo = FatIoCounters();
			EnableFatIoCounters(false);
			Print(FormatW(
				L"    free-space query: %llu disk reads, free=%llu bytes",
				static_cast<unsigned long long>(freeIo.reads),
				static_cast<unsigned long long>(freeNow)));
			check(
				L"free-space query scans the FAT in sectors (< 4000 reads)",
				freeIo.reads < 4000);
		}

		// 空闲空间必须真的减少（分配器有在记账）
		const uint64_t freeAfter = fs.freeBytes();
		check(L"free space decreased", freeAfter < kImageSize - 1024 * 1024);

		// undo 日志：写一个文件 → 回放 → 该文件消失，而先前写的文件还在
		{
			UndoJournal journal;
			fs.SetJournal(&journal);
			const bool wrote = fs.WriteFile(
				"EFI\\IceMelt\\BOOTMGW.OLD",
				64 * 1024,
				pattern(0x44),
				error);
			fs.SetJournal(nullptr);
			check(L"write with the undo journal armed", wrote);
			check(L"journal recorded metadata sectors", journal.Count() > 0);

			const std::filesystem::path undo = TempFile(L"icemelt-fat-selftest.undo");
			check(L"journal saved", journal.SaveTo(undo, /*diskNumber=*/0, error));
			size_t restored = 0;
			check(
				L"journal replay",
				UndoJournal::RestoreFrom(io, undo, restored, error));
			check(L"replay restored every chunk", restored == journal.Count());
			check(
				L"after replay the new file is gone",
				!fs.Exists("EFI\\IceMelt\\BOOTMGW.OLD", nullptr, nullptr, error));
			check(
				L"an unrelated earlier file survives the replay",
				fs.Exists("EFI\\IceMelt\\SYSTEM", nullptr, nullptr, error));
			RemoveFile(undo);
		}

		io.Close();
		Print(FormatW(L"image kept for an independent check: %ls", image.c_str()));
		Print(ok ? L"FAT SELFTEST PASS" : L"FAT SELFTEST FAIL");
		return ok ? 0 : 1;
	}

	// ------------------------------------------------------- 分区表解析自检
	//
	// 保护性 MBR 的判据只看类型字节 0xEE。曾经的判据多查了一个 CHS 字节（写成 [1]==0x02，
	// 而标准起始 CHS 是 [1]=0x00 [2]=0x02 [3]=0x00），结果在 VMware NVMe 上：
	// 保护性 MBR 判不出来 → 掉进 MBR 分支找 0xEF → "没有任何磁盘上有 ESP"，且日志里没有
	// 任何关于"到底看到了什么"的线索。下面三个用例把这条路径钉住。
	int PartitionSelfTest(const std::filesystem::path &exeDir)
	{
		(void) exeDir;
		bool ok = true;
		const auto check = [&](const wchar_t *what, bool value)
		{
			Print(FormatW(L"%-56ls: %ls", what, value ? L"OK" : L"FAILED"));
			if (!value) ok = false;
		};
		const auto WrU16 = [](uint8_t *p, uint16_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>(v >> 8);
		};
		const auto WrU32 = [](uint8_t *p, uint32_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
			p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
			p[3] = static_cast<uint8_t>(v >> 24);
		};
		const auto WrU64 = [](uint8_t *p, uint64_t v)
		{
			for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
		};

		// ESP 类型 GUID 的字节**不能由我自己写死**：如果我在解析器里把字节序写错，自检用同一份
		// 常量造镜像就会"两边同错、测试全绿"，而真盘上永远找不到 ESP。这里改由 Windows 自己的
		// GUID 解析器（rpcrt4!UuidFromStringW）把规范里的字符串转成字节 —— 基准来自系统实现，
		// 不再是我这份代码。规则：Data1/Data2/Data3 小端，Data4 按字节序。
		uint8_t kEspGuid[16] = {};
		{
			UUID uuid{};
			const wchar_t *text = L"C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
			if (::UuidFromStringW(
					reinterpret_cast<RPC_WSTR>(const_cast<wchar_t *>(text)),
					&uuid) != RPC_S_OK)
			{
				Print(L"FAIL UuidFromStringW could not parse the canonical ESP GUID");
				return 1;
			}
			std::memcpy(kEspGuid, &uuid, 16);
			check(
				L"the ESP GUID from the spec is 28 73 2A C1 ... (mixed-endian)",
				kEspGuid[0] == 0x28 && kEspGuid[1] == 0x73 && kEspGuid[2] == 0x2A &&
				kEspGuid[3] == 0xC1 && kEspGuid[15] == 0x3B);
		}
		const uint8_t kDataGuid[16] = {
			0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
			0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

		const uint64_t kEspFirstLba = 2048;
		const uint64_t kEspLastLba  = kEspFirstLba + 500ull * 1024 * 1024 / 512 - 1;

		const std::filesystem::path image = TempFile(L"icemelt-part-selftest.img");
		Print(L"--- partition table parser self-test (synthetic images) ---");

		// ---- 用例 1：保护性 MBR + GPT，含一个 ESP ----
		{
			FileSectorIo io;
			std::wstring error;
			if (!io.Open(image, /*create=*/true, 4ull * 1024 * 1024, error))
			{
				Print(FormatW(L"FAIL create image: %ls", error.c_str()));
				return 1;
			}
			uint8_t mbr[512] = {};
			uint8_t *e       = mbr + 0x1BE;
			e[0]             = 0x00; // boot indicator
			e[1]             = 0x00; // CHS head 0
			e[2]             = 0x02; // CHS sector 2  ← 事故就出在这个字节的偏移上
			e[3]             = 0x00; // CHS cylinder 0
			e[4]             = 0xEE; // protective
			e[5] = e[6] = e[7] = 0xFF;
			WrU32(e + 8, 1);
			WrU32(e + 12, 0xFFFFFFFFu);
			mbr[510] = 0x55;
			mbr[511] = 0xAA;
			io.Write(0, mbr, sizeof(mbr));

			uint8_t gpt[512] = {};
			std::memcpy(gpt, "EFI PART", 8);
			WrU32(gpt + 0x08, 0x00010000);
			WrU32(gpt + 0x0C, 92);
			WrU64(gpt + 0x18, 1);
			WrU64(gpt + 0x20, 8191);
			WrU64(gpt + 0x28, 34);
			WrU64(gpt + 0x30, 8190);
			WrU64(gpt + 0x48, 2);     // partition entry LBA
			WrU32(gpt + 0x50, 128);   // entry count
			WrU32(gpt + 0x54, 128);   // entry size
			io.Write(512, gpt, sizeof(gpt));

			uint8_t entries[128 * 128] = {};
			uint8_t *p0 = entries;
			std::memcpy(p0, kEspGuid, 16);
			WrU64(p0 + 0x20, kEspFirstLba);
			WrU64(p0 + 0x28, kEspLastLba);
			// 属性位（条目偏移 0x30 —— 在名字段 0x38 之前，不重叠；0x48 是 GPT 头的
			// "分区条目 LBA"，与条目内的属性字段完全不同）带上 hidden(0x4) 与
			// no-drive-letter(0x8000...)：DG"取消隐藏"之前的 ESP 就是这个形状。
			// 解析器必须把它报出来 —— 卷锁走不走 GLOBALROOT 兜底、直写为什么被拒，
			// 诊断全靠这两个位可见。
			WrU64(p0 + 0x30, 0x8000000000000004ull);
			// 名字字段：0x38..0x7F，36 个 UTF-16 码元定长（不与 0x30 属性位重叠）
			const wchar_t *label = L"EFI system partition";
			for (int i = 0; label[i] != 0 && i < 36; ++i)
			{
				p0[0x38 + i * 2]     = static_cast<uint8_t>(label[i] & 0xFF);
				p0[0x38 + i * 2 + 1] = static_cast<uint8_t>(label[i] >> 8);
			}
			io.Write(2 * 512, entries, sizeof(entries));

			std::vector<EspPartition>   found;
			std::vector<std::wstring>   notes;
			bool                        gptFound = false;
			bool                        mbrFound = false;
			const bool parsed = ParseEspPartitions(
				io, 0, found, gptFound, mbrFound, notes, error);
			check(L"protective MBR is recognised as GPT", parsed && gptFound && !mbrFound);
			check(L"the ESP partition is found", found.size() == 1);
			if (found.size() == 1)
			{
				check(
					L"its offset is right (LBA 2048)",
					found[0].offsetBytes == kEspFirstLba * 512);
				check(
					L"its length is right (500 MB)",
					found[0].lengthBytes == (kEspLastLba - kEspFirstLba + 1) * 512);
			}

			check(
				L"hidden + no-drive-letter attributes are reported",
				found.size() == 1 &&
					found[0].description.find(L"hidden") != std::wstring::npos &&
					found[0].description.find(L"no-drive-letter") != std::wstring::npos);
			io.Close();
		}

		// ---- 用例 2：GPT 里有分区但没有 ESP 类型 ----
		{
			FileSectorIo io;
			std::wstring error;
			if (io.Open(image, /*create=*/false, 0, error))
			{
				uint8_t entries[128 * 128] = {};
				std::memcpy(entries, kDataGuid, 16);
				WrU64(entries + 0x20, 2048);
				WrU64(entries + 0x28, 4095);
				io.Write(2 * 512, entries, sizeof(entries));

				std::vector<EspPartition> found;
				std::vector<std::wstring> notes;
				bool                      gptFound = false;
				bool                      mbrFound = false;
				const bool parsed = ParseEspPartitions(
					io, 0, found, gptFound, mbrFound, notes, error);
				check(L"a GPT without an ESP yields no candidate", parsed && found.empty());
				check(L"but it is still reported as GPT in the notes", gptFound);
				io.Close();
			}
			else
			{
				check(L"reopen for case 2", false);
			}
		}

		// ---- 用例 3：纯 MBR 盘，类型 0xEF ----
		{
			FileSectorIo io;
			std::wstring error;
			if (io.Open(image, /*create=*/true, 1024 * 1024, error))
			{
				uint8_t mbr[512] = {};
				uint8_t *e       = mbr + 0x1BE;
				e[0]             = 0x80;
				e[4]             = 0xEF;
				WrU32(e + 8, 2048);
				WrU32(e + 12, 100000);
				mbr[510] = 0x55;
				mbr[511] = 0xAA;
				io.Write(0, mbr, sizeof(mbr));

				std::vector<EspPartition> found;
				std::vector<std::wstring> notes;
				bool                      gptFound = false;
				bool                      mbrFound = false;
				const bool parsed = ParseEspPartitions(
					io, 0, found, gptFound, mbrFound, notes, error);
				check(L"an MBR ESP (type 0xEF) is found", parsed && found.size() == 1);
				check(L"and it is reported as MBR, not GPT", mbrFound && !gptFound);
				io.Close();
			}
			else
			{
				check(L"reopen for case 3", false);
			}
		}

		RemoveFile(image);
		Print(ok ? L"PARTITION SELFTEST PASS" : L"PARTITION SELFTEST FAIL");
		return ok ? 0 : 1;
	}
} // namespace icemelt
