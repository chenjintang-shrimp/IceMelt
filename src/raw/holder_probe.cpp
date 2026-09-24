#include "raw/holder_probe.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm> // std::sort, std::size
#include <iterator>  // std::size
#include <map>

#include "util.h"

namespace icemelt
{
	namespace
	{
		// NtQuerySystemInformation 的公开内核句柄表（SystemExtendedHandleInformation）。
		// SDK 头只给不带 EX 的版本，这里按 PHNT 公开布局定义；两个布局在 x86/x64 上
		// 字段顺序一致，长度自描述（NumberOfHandles 前有 Reserved）。
		using NTSTATUS_ = LONG;
		struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_
		{
			PVOID     Object;
			ULONG_PTR UniqueProcessId;
			ULONG_PTR HandleValue;
			ULONG     GrantedAccess;
			USHORT    CreatorBackTraceIndex;
			USHORT    ObjectTypeIndex;
			ULONG     HandleAttributes;
			ULONG     Reserved;
		};
		struct SYSTEM_HANDLE_INFORMATION_EX_
		{
			ULONG_PTR NumberOfHandles;
			ULONG_PTR Reserved;
		};
		using NtQuerySystemInformation_ = NTSTATUS_(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
		constexpr ULONG     kSystemExtendedHandleInformation = 64;
		constexpr NTSTATUS_ kInfoLengthMismatch = static_cast<NTSTATUS_>(0xC0000004L);

		// 归一 "\\Device\HarddiskVolumeN" 之类的名字，大小写不敏感的前缀匹配，
		// 且确保匹配边界：名字本身就是设备，或以 '\' 开头的子路径。
		bool NameWithinDevice(const wchar_t *path, const std::wstring &deviceName)
		{
			static const wchar_t *kPrefix = L"\\Device\\";
			const size_t          plen    = ::wcslen(kPrefix);
			if (_wcsnicmp(path, kPrefix, plen) != 0) return false;
			const wchar_t *rest = path + plen;
			if (_wcsnicmp(rest, deviceName.c_str(), deviceName.size()) != 0) return false;
			const wchar_t tail = rest[deviceName.size()];
			return tail == L'\0' || tail == L'\\';
		}
	} // namespace

	bool FindCoveringVolumeName(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		std::wstring &deviceName,
		std::wstring &note)
	{
		deviceName.clear();
		uint32_t opened = 0;
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
				if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND &&
					err != ERROR_NO_SUCH_DEVICE && err != ERROR_BAD_UNIT)
					note += FormatW(L"HarddiskVolume%u: open %lu; ", n, err);
				continue;
			}
			++opened;

			uint8_t buf[1024] = {};
			DWORD   bytes     = 0;
			const BOOL ok = ::DeviceIoControl(
				h,
				IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
				nullptr,
				0,
				buf,
				sizeof(buf),
				&bytes,
				nullptr);
			bool covers = false;
			if (ok && bytes >= sizeof(DWORD))
			{
				const auto *extents =
						reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf);
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
			::CloseHandle(h);
			if (covers)
			{
				deviceName = FormatW(L"HarddiskVolume%u", n);
				if (opened > 0)
					note += FormatW(L"(scanned %u volume devices)", opened);
				return true;
			}
		}
		if (opened > 0)
			note += FormatW(L"(scanned %u volume devices, none covers)", opened);
		return true;
	}

	bool ListVolumeHolders(
		const std::wstring &      name,
		std::vector<VolumeHolder> &out,
		std::wstring &            note,
		std::wstring &            error)
	{
		out.clear();
		note.clear();

		HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); // 已加载，必然成功
		if (!ntdll)
		{
			error = L"GetModuleHandle(ntdll) failed";
			return false;
		}
		const auto querySys =
				reinterpret_cast<NtQuerySystemInformation_>(
					::GetProcAddress(ntdll, "NtQuerySystemInformation"));
		if (!querySys)
		{
			error = L"GetProcAddress(NtQuerySystemInformation) failed";
			return false;
		}

		// 缓冲区按返回的 needed 翻倍直到够装。
		SIZE_T cap = 4 << 20;
		std::vector<uint8_t> buf(cap);
		ULONG needed = 0;
		for (;;)
		{
			const NTSTATUS_ st = querySys(
				kSystemExtendedHandleInformation,
				buf.data(),
				static_cast<ULONG>(cap),
				&needed);
			if (st == kInfoLengthMismatch)
			{
				cap = static_cast<SIZE_T>(needed) * 2;
				if (cap > (1u << 30)) // 1 GiB 上限：超出说明返回了不可信的大小
				{
					error = L"handle table size looks bogus";
					return false;
				}
				buf.assign(cap, 0);
				continue;
			}
			if (st < 0)
			{
				error = FormatW(L"NtQuerySystemInformation: 0x%08lX", static_cast<ULONG>(st));
				return false;
			}
			break;
		}
		const auto *info =
				reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_ *>(buf.data());
		const auto *entries = reinterpret_cast<const SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_ *>(
				buf.data() + sizeof(SYSTEM_HANDLE_INFORMATION_EX_));

		const DWORD selfPid = ::GetCurrentProcessId();
		// 同 (pid, 路径) 的句柄可能有很多条；聚合成一行 + handles 计数，否则没法读。
		struct Key
		{
			uint32_t     pid;
			std::wstring path;
			bool operator<(const Key &o) const
			{
				if (pid != o.pid) return pid < o.pid;
				return path < o.path;
			}
		};
		std::map<Key, VolumeHolder> merged;
		uint32_t skippedOwnPid = 0;
		uint32_t skippedOpenProc = 0;
		uint32_t skippedDup = 0;
		uint32_t skippedNotDiskFile = 0;
		uint32_t skippedNoPath = 0;

		for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
		{
			const auto &e   = entries[i];
			const DWORD  pid = static_cast<DWORD>(e.UniqueProcessId);
			if (pid == selfPid)
			{
				++skippedOwnPid;
				continue;
			}
			// 受保护进程开不了（最常见：受保护的杀软、System 的某些对象）。
			HANDLE proc = ::OpenProcess(
				PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!proc)
			{
				++skippedOpenProc;
				continue;
			}
			HANDLE local = nullptr;
			const BOOL dupped = ::DuplicateHandle(
				proc,
				reinterpret_cast<HANDLE>(e.HandleValue),
				::GetCurrentProcess(),
				&local,
				0,
				FALSE,
				DUPLICATE_SAME_ACCESS);
			if (!dupped)
			{
				++skippedDup;
				::CloseHandle(proc);
				continue;
			}
			// 只对世界上的"真文件/卷"句柄问名字：管道/套接字/控制台的 Name 查询
			// 在对方同步 IO 挂起时会把我们拖住。GetFileType 不查对象名，不会挂。
			const DWORD type = ::GetFileType(local);
			if (type != FILE_TYPE_DISK)
			{
				++skippedNotDiskFile;
				::CloseHandle(local);
				::CloseHandle(proc);
				continue;
			}
			wchar_t     pathBuf[32768];
			const DWORD n = ::GetFinalPathNameByHandleW(
				local,
				pathBuf,
				static_cast<DWORD>(std::size(pathBuf)),
				VOLUME_NAME_NT | FILE_NAME_NORMALIZED);
			if (n == 0 || n >= std::size(pathBuf))
			{
				++skippedNoPath;
				::CloseHandle(local);
				::CloseHandle(proc);
				continue;
			}
			if (NameWithinDevice(pathBuf, name))
			{
				const Key key{pid, pathBuf};
				auto      it = merged.find(key);
				if (it == merged.end())
				{
					VolumeHolder h{};
					h.pid        = pid;
					h.objectPath = pathBuf;
					h.access     = e.GrantedAccess;
					wchar_t procName[MAX_PATH];
					DWORD   sz = static_cast<DWORD>(std::size(procName));
					if (::QueryFullProcessImageNameW(proc, 0, procName, &sz))
						h.processName = procName;
					h.handles = 1;
					merged.emplace(key, h);
				}
				else
				{
					++it->second.handles;
				}
			}
			::CloseHandle(local);
			::CloseHandle(proc);
		}

		out.reserve(merged.size());
		for (auto &kv: merged) out.push_back(kv.second);
		std::sort(out.begin(), out.end(), [](const VolumeHolder &a, const VolumeHolder &b) {
			if (a.pid != b.pid) return a.pid < b.pid;
			return a.objectPath < b.objectPath;
		});
		note = FormatW(
			L"skipped: own-pid %u, open-process %u, dup %u, not-a-disk-file %u, no-path %u",
			skippedOwnPid,
			skippedOpenProc,
			skippedDup,
			skippedNotDiskFile,
			skippedNoPath);
		return true;
	}
} // namespace icemelt
