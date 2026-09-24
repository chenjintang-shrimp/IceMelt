#include "deploy/esp_deploy.h"

#include <windows.h>

#include <vector>

#include "melt/unfreeze.h" // kLoaderServiceName
#include "raw/ntfs_tool.h" // RunTool
#include "reg/hive.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

namespace icemelt
{
	namespace fs = std::filesystem;
	namespace
	{
		constexpr wchar_t kEspBootDir[] = L"EFI\\Microsoft\\Boot";

		// 挂载点 RAII：无论哪一步失败，析构时都把盘符卸掉。mountvol 的失败只记录，
		// 不抛状态 —— 部署已经失败的前提下，卸载再失败不应掩盖真正的错误。
		struct EspMount
		{
			wchar_t letter = 0; // 0 = 未挂载

			~EspMount()
			{
				if (letter == 0) return;
				int          code = -1;
				std::wstring out;
				RunTool(MountvolPath(), FormatW(L"%lc: /D", letter), code, out);
			}

			static std::filesystem::path MountvolPath()
			{
				wchar_t sysdir[MAX_PATH] = {};
				const UINT n = ::GetSystemDirectoryW(sysdir, MAX_PATH);
				if (n == 0 || n >= MAX_PATH) return L"mountvol";
				return std::filesystem::path(sysdir) / L"mountvol.exe";
			}
		};

		// 找一个空闲盘符：从 Z 往 D 倒着挑，避开常见的 C/D 与网络盘聚集区。
		wchar_t FreeDriveLetter()
		{
			const DWORD used = ::GetLogicalDrives();
			for (wchar_t c = L'Z'; c >= L'D'; --c)
			{
				if ((used & (1u << (c - L'A'))) == 0) return c;
			}
			return 0;
		}

		// 文件大小比对用 GetFileAttributesExW：与 util.h 的 Win7 约束同源 ——
		// std::filesystem::file_size 会把 CreateFile2（Win8+）拉进导入表。
		bool SameFileSize(
			const std::filesystem::path &a,
			const std::filesystem::path &b)
		{
			WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
			if (!::GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa))
				return false;
			if (!::GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb))
				return false;
			const ULONGLONG sa =
					(static_cast<ULONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
			const ULONGLONG sb =
					(static_cast<ULONGLONG>(fb.nFileSizeHigh) << 32) | fb.nFileSizeLow;
			return sa == sb;
		}

		bool CopyOne(
			const std::filesystem::path &src,
			const std::filesystem::path &dst,
			std::vector<std::wstring> & log)
		{
			if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE))
			{
				log.push_back(
					FormatW(
						L"[x] copy failed: %ls -> %ls (error %lu)",
						src.filename().c_str(),
						dst.c_str(),
						::GetLastError()));
				return false;
			}
			// 落盘即读回大小：ESP 是 FAT，普通路径写不会"报成功但没写进去"，
			// 但大小比对便宜，顺手把截断/错拷这类事故挡在重启之前。
			if (!SameFileSize(src, dst))
			{
				log.push_back(
					FormatW(
						L"[x] size mismatch after copy: %ls",
						dst.c_str()));
				return false;
			}
			log.push_back(
				FormatW(
					L"[+] %ls -> %ls",
					src.filename().c_str(),
					dst.c_str()));
			return true;
		}
		// 临时文件路径（%TEMP% 不是 std::filesystem::temp_directory_path：后者经
		// GetTempPath2 一类的较新入口，Win7 上没有）。
		std::filesystem::path TempFile(const wchar_t *name)
		{
			wchar_t dir[MAX_PATH] = {};
			const DWORD n = ::GetTempPathW(MAX_PATH, dir);
			if (n == 0 || n >= MAX_PATH) return std::filesystem::path(name);
			return std::filesystem::path(dir) / name;
		}

		// 现场生产 payload：导出本机 SYSTEM → 离线摘除名单（服务键 + 设备类过滤器项）→
		// 置干净/修校验和/可加载性校验。返回空路径 = 失败（log 里已写明原因）。
		//
		// 为什么是"现场导出"而不是收一份静态文件：要摘掉的正是**这台机器**上的冻结产品
		// 服务键与过滤器引用；换一台机器、或冻结产品换了版本，静态 payload 就是错的。
		std::filesystem::path ProducePayload(
			const std::filesystem::path &exeDir,
			std::vector<std::wstring> &   log)
		{
			const auto targets = LoadTargets(exeDir);
			auto       names   = TargetNames(targets);
			if (names.empty())
			{
				log.push_back(L"[x] the target list is empty: nothing to strip, refusing to deploy");
				return {};
			}
			log.push_back(
				FormatW(L"[*] target list: %zu name(s) to strip", names.size()));

			const std::filesystem::path hive = TempFile(L"icemelt-system.hive");
			RemoveFile(hive); // RegSaveKeyEx 在目标已存在时失败

			std::wstring error;
			if (!ExportSystemHive(hive, error))
			{
				log.push_back(FormatW(L"[x] export SYSTEM hive: %ls", error.c_str()));
				return {};
			}
			log.push_back(FormatW(L"[+] exported live SYSTEM hive to %ls", hive.c_str()));

			// 服务键 = 名单里的服务 + 装载器自己的临时服务；过滤器项 = 名单。
			std::vector<std::wstring> services = names;
			services.push_back(kLoaderServiceName);

			EditReport report;
			if (!StripTargetsInExportedHive(hive, services, names, report, error))
			{
				log.push_back(FormatW(L"[x] offline strip failed: %ls", error.c_str()));
				return {};
			}
			log.push_back(
				FormatW(
					L"[+] stripped %zu service key(s) and %zu filter value(s) from the payload",
					report.deletedKeys.size(),
					report.changedValues.size()));
			for (const auto &k: report.deletedKeys)
				log.push_back(FormatW(L"    service key removed: %ls", k.c_str()));
			for (const auto &v: report.changedValues)
				log.push_back(FormatW(L"    filter value rewritten: %ls", v.c_str()));

			HiveBaseBlock block;
			if (!MakeCleanAndFixChecksum(hive, block, error))
			{
				log.push_back(FormatW(L"[x] clean/fix checksum: %ls", error.c_str()));
				return {};
			}
			if (!VerifyHiveLoadable(hive, error))
			{
				log.push_back(FormatW(L"[x] payload is not loadable: %ls", error.c_str()));
				return {};
			}
			log.push_back(
				FormatW(
					L"[+] payload verified: clean, checksum valid, root cell ok (%llu bytes)",
					static_cast<unsigned long long>(block.fileSize)));
			return hive;
		}
	} // namespace

	DeployResult DeployEsp(const std::filesystem::path &exeDir)
	{
		DeployResult r;
		auto &       log = r.log;

		if (!IsProcessElevated())
		{
			log.push_back(L"[x] NOT elevated: mountvol /S and ESP writes need an admin token");
			log.push_back(L"    -> relaunch from an elevated prompt (Run as administrator)");
			return r;
		}

		// ---- 1. 资产清点（挂载前就失败，不留半截部署）----
		const fs::path src = exeDir / L"gunpowder";
		struct Asset
		{
			const wchar_t *file;
			const wchar_t *why;
		};
		static constexpr Asset kAssets[] = {
			{L"shellx64.efi", L"UEFI Shell, becomes the replacement bootmgfw.efi"},
			{L"ntfs-3g.efi", L"NTFS driver loaded by startup.nsh"},
			{L"startup.nsh", L"pre-boot melt script"},
		};
		bool missing = false;
		for (const auto &a: kAssets)
		{
			if (!PathIsRegularFile(src / a.file))
			{
				log.push_back(
					FormatW(L"[x] missing asset: gunpowder\\%ls (%ls)", a.file, a.why));
				missing = true;
			}
		}
		if (missing)
		{
			log.push_back(L"    -> place the files under <exe>\\gunpowder and rerun");
			return r;
		}

		// ---- 1b. 现场生产 payload（只读导出 + 离线编辑一份副本；不碰 ESP，不碰活动注册表）----
		const std::filesystem::path payload = ProducePayload(exeDir, log);
		if (payload.empty()) return r;

		// ---- 2. 挂出 ESP ----
		const wchar_t letter = FreeDriveLetter();
		if (letter == 0)
		{
			log.push_back(L"[x] no free drive letter for the ESP mount");
			return r;
		}
		EspMount     mount;
		int          code = -1;
		std::wstring out;
		if (!RunTool(EspMount::MountvolPath(), FormatW(L"%lc: /S", letter), code, out) ||
			code != 0)
		{
			log.push_back(
				FormatW(L"[x] mountvol %lc: /S failed (exit %d)", letter, code));
			return r;
		}
		mount.letter = letter;
		const fs::path esp = FormatW(L"%lc:\\", letter);
		log.push_back(FormatW(L"[+] ESP mounted at %lc:", letter));

		// 确认这确实是一个有 Windows 引导目录的 ESP，再往里面写。
		const fs::path bootDir = esp / kEspBootDir;
		if (!PathIsDirectory(bootDir))
		{
			log.push_back(
				FormatW(L"[x] %ls does not exist: not a Windows ESP?", bootDir.c_str()));
			return r;
		}
		const fs::path iceDir = esp / L"EFI" / L"IceMelt";
		if (!PathIsDirectory(iceDir) &&
			!::CreateDirectoryW(iceDir.c_str(), nullptr) &&
			::GetLastError() != ERROR_ALREADY_EXISTS)
		{
			log.push_back(
				FormatW(
					L"[x] cannot create %ls (error %lu)",
					iceDir.c_str(),
					::GetLastError()));
			return r;
		}

		// ---- 3. 原始 bootmgfw 备份：只建一次（重复部署时 bootmgfw 已经是我们的 shell，
		// 没有守卫会把 shell 当成"原始文件"备份，真正的 Windows 引导器就丢了）----
		const fs::path espBootmgfw = bootDir / L"bootmgfw.efi";
		const fs::path backup      = iceDir / L"bootmgfw-original.efi";
		if (!PathExists(backup))
		{
			if (!PathIsRegularFile(espBootmgfw))
			{
				log.push_back(
					FormatW(
						L"[x] %ls not found and no backup exists: cannot deploy safely",
						espBootmgfw.c_str()));
				return r;
			}
			if (!::CopyFileW(espBootmgfw.c_str(), backup.c_str(), TRUE))
			{
				log.push_back(
					FormatW(
						L"[x] bootmgfw backup failed (error %lu)",
						::GetLastError()));
				return r;
			}
			log.push_back(L"[+] original bootmgfw.efi backed up to \\EFI\\IceMelt");
		}
		else
		{
			log.push_back(L"[*] bootmgfw backup already present, kept as-is");
		}

		// ---- 4. 铺文件 ----
		bool copied = true;
		copied &= CopyOne(src / L"shellx64.efi", espBootmgfw, log);
		copied &= CopyOne(src / L"startup.nsh", bootDir / L"startup.nsh", log);
		copied &= CopyOne(src / L"ntfs-3g.efi", iceDir / L"ntfs-3g.efi", log);
		copied &= CopyOne(payload, iceDir / L"SYSTEM", log);
		if (!copied)
		{
			log.push_back(
				FormatW(
					L"[x] deploy incomplete; ESP mount will be removed. The payload hive is kept at "
					L"%ls for inspection",
					payload.c_str()));
			return r;
		}
		// 成功才删：失败时那份 hive 是排查材料（和 melt 的 hiveOutPath 一样，它含有本机
		// SYSTEM 内容，不该长期留在 %TEMP%）。
		RemoveFile(payload);

		log.push_back(
			L"[+] deploy complete: next boot runs startup.nsh (Secure Boot must be OFF)");
		log.push_back(
			L"[*] startup.nsh restores the original bootmgfw after writing the hive");
		// 部署成功不等于会生效：这一步只保证 ESP 上的文件就位，能不能被加载取决于 BCD。
		// 设备指向 Windows 分区、启动文件是 \Windows\system32\winload.efi 的条目压根不经过
		// bootmgfw.efi —— 那种机器必须先修引导（WinRE 里 bcdboot），否则重启后什么都没发生。
		log.push_back(
			L"[!] prerequisite: the firmware must actually boot \\EFI\\Microsoft\\Boot\\bootmgfw.efi");
		log.push_back(
			L"[!] from this ESP. If the BCD entry boots \\Windows\\system32\\winload.efi on the");
		log.push_back(
			L"[!] Windows partition instead, this deploy never runs -- repair the entry in WinRE");
		log.push_back(
			L"[!] (bcdboot C:\\Windows /s S: /f UEFI, S: = the ESP) and re-run --deploy-esp");
		r.ok = true;
		return r;
	}
} // namespace icemelt
