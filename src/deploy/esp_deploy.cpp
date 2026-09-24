#include "deploy/esp_deploy.h"

#include <windows.h>

#include <cwctype>
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

	// 一个可作为载体的 FAT 卷（FAT12/16 报 "FAT"，FAT32 报 "FAT32"）
	struct FatVolume
	{
		wchar_t      letter = 0;
		std::wstring label;
		std::wstring fsName;
		bool         removable = false;
	};

	// 枚举 FAT12/16/32 卷。removableOnly=false 时也返回固定盘（显式 --drive= 指定时才用，
	// 因为"替用户挑一个固定盘写引导"不是能自动做的决定）。
	std::vector<FatVolume> FindFatVolumes(bool removableOnly)
	{
		std::vector<FatVolume> out;
		const DWORD            mask = ::GetLogicalDrives();
		for (wchar_t c = L'A'; c <= L'Z'; ++c)
		{
			if ((mask & (1u << (c - L'A'))) == 0) continue;
			const std::wstring root = FormatW(L"%lc:\\", c);

			const UINT type = ::GetDriveTypeW(root.c_str());
			// 跳过光驱/网络盘/未就绪：它们要么只读，要么根本不是"可写的 FAT 载体"
			if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;

			wchar_t label[MAX_PATH]  = {};
			wchar_t fsName[MAX_PATH] = {};
			DWORD   serial = 0, maxComp = 0, flags = 0;
			if (!::GetVolumeInformationW(
					root.c_str(),
					label,
					MAX_PATH,
					&serial,
					&maxComp,
					&flags,
					fsName,
					MAX_PATH))
				continue; // 无文件系统（RAW）或未就绪
			if ((flags & FILE_READ_ONLY_VOLUME) != 0) continue;

			const std::wstring fs = fsName;
			if (fs != L"FAT" && fs != L"FAT32") continue; // FAT12/16 统一报 "FAT"

			FatVolume v;
			v.letter    = c;
			v.label     = label;
			v.fsName    = fs;
			v.removable = (type == DRIVE_REMOVABLE);
			if (removableOnly && !v.removable) continue;
			out.push_back(v);
		}
		return out;
	}

	// 覆盖前把 U 盘上原有的同名文件挪进备份目录；已经有备份就不动（重复部署不会用我们的
	// shell 覆盖掉真正的原始引导）。
	bool BackupOnce(
		const std::filesystem::path &src,
		const std::filesystem::path &backup,
		std::vector<std::wstring> &   log)
	{
		if (PathExists(backup)) return true;
		if (!PathIsRegularFile(src)) return true; // 原本没有这个文件：没有要备份的东西
		if (!::CopyFileW(src.c_str(), backup.c_str(), TRUE))
		{
			log.push_back(
				FormatW(
					L"[x] backup failed: %ls -> %ls (error %lu)",
					src.c_str(),
					backup.c_str(),
					::GetLastError()));
			return false;
		}
		log.push_back(FormatW(L"[+] backed up %ls", src.c_str()));
		return true;
	}

	// 0 字节标记文件：startup.nsh 靠它区分"从 U 盘引导"（要提示拔盘）与"从 ESP 引导"。
	bool WriteMarker(const std::filesystem::path &path, std::wstring &error)
	{
		HANDLE h = ::CreateFileW(
			path.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = FormatW(
				L"CreateFileW(%ls) failed: %lu",
				path.c_str(),
				::GetLastError());
			return false;
		}
		::CloseHandle(h);
		return true;
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

		// payload 是"导出运行中系统的 HKLM\\SYSTEM"。在 WinRE/WinPE 里那个键是恢复环境自己的
		// 注册表（X:\\Windows\\...），铺进 ESP 就等于把 WinRE 的 hive 装到目标机上 —— 机器直接
		// 起不来。必须在任何导出动作之前拦住。
		if (IsWindowsPreinstallationEnvironment())
		{
			log.push_back(
				L"[x] running inside WinPE/WinRE: HKLM\\SYSTEM here is the recovery environment's");
			log.push_back(
				L"    own hive, not the target system's -- the payload would brick the target");
			log.push_back(
				L"    -> run --deploy-esp from the target Windows installation itself");
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

		// ---- 3. 原始引导器备份：只建一次（重复部署时那两个文件已经是我们的 shell，
		// 没有守卫会把 shell 当成"原始文件"备份，真正的 Windows 引导器就丢了）----
		//
		// 两个位置都要覆盖，因为它们对应固件的两条不同路径：
		//   \EFI\Microsoft\Boot\bootmgfw.efi —— NVRAM 的 Boot#### 条目通常指向它（首选路径）
		//   \EFI\Boot\BOOTX64.EFI           —— 所有 Boot#### 都失败/不存在时的默认文件路径
		//                                      （UEFI 规范的可移动介质默认路径；Windows 也会在
		//                                      这里放一份 bootmgfw 的副本）
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

		const fs::path espBootx64  = esp / L"EFI" / L"Boot" / L"BOOTX64.EFI";
		const fs::path backupX64   = iceDir / L"bootx64-original.efi";
		bool           coverFallback = false;
		if (PathExists(backupX64))
		{
			coverFallback = true;
			log.push_back(L"[*] bootx64 backup already present, kept as-is");
		}
		else if (PathIsRegularFile(espBootx64))
		{
			if (!::CopyFileW(espBootx64.c_str(), backupX64.c_str(), TRUE))
			{
				log.push_back(
					FormatW(
						L"[x] \\EFI\\Boot\\BOOTX64.EFI backup failed (error %lu)",
						::GetLastError()));
				return r;
			}
			coverFallback = true;
			log.push_back(L"[+] \\EFI\\Boot\\BOOTX64.EFI backed up to \\EFI\\IceMelt");
		}
		else
		{
			// 不凭空创建它：那会把固件的"无引导项"回退路径改成指向我们的 shell，而这条
			// 路径本来就是"连 Windows 都起不来"时才走。只如实说明覆盖范围。
			log.push_back(
				L"[!] no \\EFI\\Boot\\BOOTX64.EFI on this ESP: only the NVRAM Boot#### route is "
				L"covered");
		}

		// ---- 4. 铺文件 ----
		bool copied = true;
		copied &= CopyOne(src / L"shellx64.efi", espBootmgfw, log);
		if (coverFallback) copied &= CopyOne(src / L"shellx64.efi", espBootx64, log);
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
			L"[*] startup.nsh restores the original boot loaders after writing the hive");
		// 部署成功不等于会生效：这一步只保证 ESP 上的文件就位。固件不读 BCD（BCD 是
		// bootmgfw 加载后自己读的），它按 NVRAM 的 BootOrder 逐条启动 —— Windows 的条目
		// 通常指向 \EFI\Microsoft\Boot\bootmgfw.efi；只有在所有条目都失败/不存在时，才会
		// 走默认文件路径 \EFI\Boot\BOOTX64.EFI（UEFI 规范的可移动介质默认路径）。
		// 我们替换的正是这两个文件，覆盖两条路径；剩下的前提是这两条路里至少有一条会被走到。
		log.push_back(
			L"[!] prerequisite: the firmware must load one of the two files we replaced:");
		log.push_back(
			L"[!]   a) NVRAM Boot#### -> \\EFI\\Microsoft\\Boot\\bootmgfw.efi (the usual route)");
		log.push_back(
			L"[!]   b) the default fallback \\EFI\\Boot\\BOOTX64.EFI (only when every Boot#### fails)");
		log.push_back(
			L"[!] show the firmware entries with: bcdedit /enum firmware");
		r.ok = true;
		return r;
	}

	DeployResult DeployUsb(
		const std::filesystem::path &exeDir,
		const std::wstring &         preferredDrive)
	{
		DeployResult r;
		auto &       log = r.log;

		if (!IsProcessElevated())
		{
			log.push_back(L"[x] NOT elevated: writing to a raw volume needs an admin token");
			log.push_back(L"    -> relaunch from an elevated prompt (Run as administrator)");
			return r;
		}
		// payload 是"导出运行中系统的 HKLM\\SYSTEM"。在 WinRE/WinPE 里那是恢复环境自己的
		// 注册表，铺上 U 盘再盖到目标机 = 起不来。见 util.h 的同一段说明。
		if (IsWindowsPreinstallationEnvironment())
		{
			log.push_back(
				L"[x] running inside WinPE/WinRE: HKLM\\SYSTEM here is the recovery environment's");
			log.push_back(
				L"    own hive, not the target system's -- the payload would brick the target");
			log.push_back(
				L"    -> run --deploy-usb from the target Windows installation itself");
			return r;
		}

		// ---- 1. 资产清点 ----
		const fs::path src = exeDir / L"gunpowder";
		static constexpr const wchar_t *kAssets[] = {
			L"shellx64.efi", L"ntfs-3g.efi", L"startup.nsh"};
		bool missing = false;
		for (const wchar_t *name: kAssets)
		{
			if (!PathIsRegularFile(src / name))
			{
				log.push_back(FormatW(L"[x] missing asset: gunpowder\\%ls", name));
				missing = true;
			}
		}
		if (missing)
		{
			log.push_back(L"    -> place the files under <exe>\\gunpowder and rerun");
			return r;
		}

		// ---- 2. 挑卷 ----
		// 显式指定：允许固定盘（有的 U 盘/读卡器枚举成 fixed），但要求必须是 FAT 且可写。
		// 不指定：只认可移动 FAT 卷，且必须恰好一个 —— "替你挑一个固定盘写引导"不是能自动
		// 做的决定。
		FatVolume target;
		if (!preferredDrive.empty())
		{
			const wchar_t letter = static_cast<wchar_t>(::towupper(preferredDrive[0]));
			bool found = false;
			for (const auto &v: FindFatVolumes(/*removableOnly=*/false))
			{
				if (v.letter != letter) continue;
				target = v;
				found  = true;
			}
			if (!found)
			{
				log.push_back(
					FormatW(
						L"[x] %lc: is not a writable FAT12/16/32 volume (or it is not mounted)",
						letter));
				return r;
			}
			if (!target.removable)
			{
				log.push_back(
					FormatW(
						L"[!] %lc: is a FIXED disk (not removable) -- continuing because it was "
						L"named explicitly",
						letter));
			}
		}
		else
		{
			const auto volumes = FindFatVolumes(/*removableOnly=*/true);
			if (volumes.empty())
			{
				log.push_back(
					L"[x] no removable FAT12/16/32 volume found: plug in one USB stick and rerun");
				log.push_back(
					L"    -> a stick enumerating as a fixed disk must be named: --drive=E:");
				return r;
			}
			if (volumes.size() > 1)
			{
				log.push_back(
					L"[x] more than one removable FAT volume -- name the one you mean:");
				for (const auto &v: volumes)
					log.push_back(
						FormatW(L"      %lc:  %ls", v.letter, v.label.c_str()));
				log.push_back(L"    -> rerun with --drive=<letter>:");
				return r;
			}
			target = volumes.front();
		}

		log.push_back(
			FormatW(
				L"[*] target volume: %lc:  %ls (%ls%s)",
				target.letter,
				target.label.c_str(),
				target.fsName.c_str(),
				target.removable ? L", removable" : L", fixed"));
		log.push_back(
			L"[!] the stick will carry a copy of this machine's SYSTEM hive (local accounts and "
			L"secrets included) -- keep it safe, and re-run --deploy-usb after use");

		// ---- 3. 现场生产 payload（只读导出 + 离线编辑副本）----
		const std::filesystem::path payload = ProducePayload(exeDir, log);
		if (payload.empty()) return r;

		// ---- 4. 布局：U 盘走可移动介质默认路径，与 ESP 模式不同 ----
		const fs::path root      = FormatW(L"%lc:\\", target.letter);
		const fs::path bootDir   = root / L"EFI" / L"BOOT";
		const fs::path iceDir    = root / L"EFI" / L"IceMelt";
		const fs::path backupDir = iceDir / L"backup";

		for (const auto &dir: {bootDir, iceDir, backupDir})
		{
			if (!PathIsDirectory(dir) && !::CreateDirectoryW(dir.c_str(), nullptr) &&
				::GetLastError() != ERROR_ALREADY_EXISTS)
			{
				log.push_back(
					FormatW(
						L"[x] cannot create %ls (error %lu)",
						dir.c_str(),
						::GetLastError()));
				return r;
			}
		}

		// ---- 5. 覆盖前先备份 U 盘原有的引导文件 ----
		// 放在独立子目录：startup.nsh 只从 \EFI\IceMelt\ 顶层认 bootmgfw-original.efi /
		// bootx64-original.efi（那两个是 ESP 模式专用的恢复源），U 盘这儿的备份不该被它
		// 当成"要恢复回去的引导器"而覆盖掉我们自己的 shell。
		if (!BackupOnce(bootDir / L"BOOTX64.EFI", backupDir / L"BOOTX64.EFI.orig", log) ||
			!BackupOnce(bootDir / L"startup.nsh", backupDir / L"startup.nsh.orig", log))
		{
			return r;
		}

		// ---- 6. 铺文件 ----
		bool copied = true;
		copied &= CopyOne(src / L"shellx64.efi", bootDir / L"BOOTX64.EFI", log);
		copied &= CopyOne(src / L"startup.nsh", bootDir / L"startup.nsh", log);
		copied &= CopyOne(src / L"ntfs-3g.efi", iceDir / L"ntfs-3g.efi", log);
		copied &= CopyOne(payload, iceDir / L"SYSTEM", log);

		std::wstring markerError;
		if (!WriteMarker(iceDir / L"boot-source-usb", markerError))
		{
			log.push_back(
				FormatW(L"[x] cannot write the USB marker: %ls", markerError.c_str()));
			copied = false;
		}
		if (!copied)
		{
			log.push_back(
				FormatW(
					L"[x] deploy incomplete; the payload hive is kept at %ls for inspection",
					payload.c_str()));
			return r;
		}
		RemoveFile(payload);

		log.push_back(FormatW(L"[+] deploy complete on %lc:", target.letter));
		log.push_back(
			L"[*] next step, on the target machine: reboot into the firmware boot menu (BBS/F12),");
		log.push_back(
			L"    pick the USB device, and let startup.nsh run. Secure Boot must be OFF.");
		log.push_back(
			L"[*] startup.nsh writes the payload to the internal Windows volume and then reboots;");
		log.push_back(
			L"    unplug the stick when it says so (it waits 15 s) so the machine boots the disk");
		log.push_back(
			L"    -- otherwise the firmware may boot the stick again and just repeats the run");
		log.push_back(
			L"[*] nothing on any internal disk was modified by this command");
		r.ok = true;
		return r;
	}
} // namespace icemelt
