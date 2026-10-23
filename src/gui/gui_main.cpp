// IceMelt —— 图形前端（Dear ImGui · Win32 + Direct3D9）
//
// 设计取舍：
//   * 与 TUI（src/main.cpp）共用同一条 pipeline（melt/unfreeze.h），不包壳子进程；
//   * 日志经 MeltLogger 回调流入环形缓冲（worker 线程 push、UI 线程 drain）；
//   * 落盘确认经 MeltOptions::confirm 回调挂到 ImGui 模态框（condvar 等回答）；
//   * 三个动作与 CLI 一一对应：预检扫描 = --preflight、只读预演 = --dry-run、
//     执行 MELT = --melt --yes-i-know（键入 MELT 就是那张 --yes-i-know）。
//
// 平台约束：Windows 7 SP1 → Windows 11。后端选 D3D9：Vista 起随系统提供，
// 无可再发行组件。本文件里禁止 std::filesystem 的"打开句柄"系调用 —— MSVC/clang-cl
// 的 STL 会把它们链到 CreateFile2(Win8+)，Win7 上启动即失败；文件/目录谓词一律走
// util.h 的 PathIs* / RemoveFile。
//
// 安全边界：GUI 启动本身只做只读探测（注册表名单 + 资产清点），不装载驱动、不碰盘；
// 驱动装载、裸盘写入只发生在用户显式点击之后。非提权时三个动作按钮全部禁用。

#include <windows.h>
#include <d3d9.h>
// WIN32_LEAN_AND_MEAN 把 winver.h 挡在外面；顶栏显示的版本直接读 exe 自身的版本资源
#include <winver.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <condition_variable>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "melt/unfreeze.h"
#include "reg/reg_edit.h"
#include "reg/targets.h"
#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND   hWnd,
	UINT   msg,
	WPARAM wParam,
	LPARAM lParam);

namespace
{
	namespace fs = std::filesystem;

	// ---- 任务模型 ---------------------------------------------------------------

	enum class JobKind { None, Preflight, DryRun, Melt };

	const char *JobName(JobKind kind)
	{
		switch (kind)
		{
			case JobKind::Preflight:
				return "预检扫描";
			case JobKind::DryRun:
				return "只读预演";
			case JobKind::Melt:
				return "执行 MELT";
			default:
				return "空闲";
		}
	}

	// worker 线程与 UI 线程之间唯一的共享状态。所有字段都在 mu 下访问；
	// 条件变量只用于 confirm 的一问一答。
	struct Shared
	{
		std::mutex               mu;
		std::condition_variable  cv;
		std::deque<std::string>  lines;                // 日志行（UTF-8），封顶 4000
		size_t                   totalLines       = 0; // 单调递增，UI 侧据此追赶
		bool                     confirmRequested = false;
		bool                     confirmAnswered  = false;
		bool                     confirmAnswer    = false;
		std::vector<std::string> pending; // 待确认清单（第 7 步）
		bool                     done = false;
		icemelt::MeltResult      result;
	};

	struct AssetRow
	{
		const char *   label;
		const wchar_t *rel;
		bool           present = false;
	};

	struct TargetRowUI
	{
		std::string name;
		std::string label;
		std::string status;
		bool        present = false;
	};

	// 名单文件的观测戳：用于"文件一变就重载"（大小 + 修改时间；不存在时 valid=false）
	struct FileStamp
	{
		bool      valid = false;
		ULONGLONG size  = 0;
		ULONGLONG mtime = 0;

		bool operator!=(const FileStamp &other) const
		{
			return valid != other.valid || size != other.size || mtime != other.mtime;
		}
	};

	struct App
	{
		HWND  hwnd     = nullptr;
		float dpiScale = 1.0f;
		ImFont *fontMono = nullptr;
		ImFont *fontTitle = nullptr;

		bool                     elevated = false;
		std::string              version; // 取自 exe 版本资源，见 VersionString()
		std::string              osVersion;
		std::string              exeDirUtf8;
		std::vector<AssetRow>    assets;
		std::vector<TargetRowUI> targets;

		// 名单来源与变更观测（动态加载：按钮 + 文件变更自动重载）
		fs::path  targetsFile; // 空 = 未找到 targets.txt，用的是内置默认名单
		FileStamp targetsStamp;
		double    nextTargetsCheck = 0.0;

		JobKind kind          = JobKind::None;
		JobKind lastKind      = JobKind::None;
		bool    running       = false;
		bool    lastOk        = false;
		bool    lastWroteDisk = false;

		// confirm 模态框
		bool                     confirmOpen = false;
		std::vector<std::string> confirmItems;
		char                     confirmBuf[32] = {};

		bool        rebootModal = false;
		std::string rebootError;

		// 日志视图（UI 线程私有）
		std::vector<std::string> lines;
		size_t                   seen   = 0;
		bool                     follow = true;

		std::thread worker;
		Shared      shared;
	};

	App *g_app = nullptr;

	// ---- 只读探测（与 src/main.cpp 保持一致）-------------------------------------

	void UiLog(App &app, const std::string &line); // 定义在"日志管线"一节

	std::wstring RegReadString(HKEY root, const wchar_t *subkey, const wchar_t *name)
	{
		wchar_t buf[512] = {};
		DWORD   bytes    = sizeof(buf);
		DWORD   type     = 0;
		if (::RegGetValueW(
			root,
			subkey,
			name,
			RRF_RT_REG_SZ,
			&type,
			buf,
			&bytes) != ERROR_SUCCESS)
			return {};
		return std::wstring(buf, bytes / sizeof(wchar_t) - 1);
	}

	std::string WindowsVersionString()
	{
		constexpr wchar_t kCurrentVersion[] =
				L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
		std::wstring product = RegReadString(
			HKEY_LOCAL_MACHINE,
			kCurrentVersion,
			L"ProductName");
		const std::wstring build = RegReadString(
			HKEY_LOCAL_MACHINE,
			kCurrentVersion,
			L"CurrentBuildNumber");
		const std::wstring ubr = RegReadString(
			HKEY_LOCAL_MACHINE,
			kCurrentVersion,
			L"UBR");

		unsigned long buildNumber = 0;
		if (!build.empty())
		{
			try
			{
				buildNumber = std::stoul(build);
			} catch (...)
			{
				buildNumber = 0;
			}
		}
		// ProductName 自 Win10 起不再更新（Win11 上仍写 "Windows 10"），按 build 号纠正
		if (buildNumber >= 22000)
		{
			constexpr wchar_t kWin10[]  = L"Windows 10";
			constexpr size_t  kWin10Len = (sizeof(kWin10) / sizeof(wchar_t)) - 1;
			if (product.compare(0, kWin10Len, kWin10) == 0)
				product = std::wstring(L"Windows 11") + product.substr(kWin10Len);
		}

		std::string version = icemelt::Narrow(product);
		if (!build.empty())
		{
			version += "  build " + icemelt::Narrow(build);
			if (!ubr.empty()) version += "." + icemelt::Narrow(ubr);
		}
		return version.empty() ? "unknown" : version;
	}

	// 本程序的版本号：取自 exe 自己的版本资源，不另设常量。xmake.lua 的 app_version 是
	// 唯一版本源（CI 在 configure 阶段用 tag 覆盖），它生成 version-gui.rc 编进 exe，
	// 所以读资源永远不会和发布包名脱节。走 VS_FIXEDFILEINFO 而不是 StringFileInfo：
	// 固定块与 Translation 无关，不依赖 040904b0 语言块是否存在。
	std::string VersionString()
	{
		wchar_t path[MAX_PATH] = {};
		if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return {};

		DWORD       ignored = 0;
		const DWORD size    = ::GetFileVersionInfoSizeW(path, &ignored);
		if (size == 0) return {};

		std::vector<unsigned char> buf(size);
		if (!::GetFileVersionInfoW(path, 0, size, buf.data())) return {};

		VS_FIXEDFILEINFO *info = nullptr;
		UINT              len  = 0;
		if (!::VerQueryValueW(
			buf.data(),
			L"\\",
			reinterpret_cast<void **>(&info),
			&len) ||
			info == nullptr || len < sizeof(VS_FIXEDFILEINFO))
			return {};
		if (info->dwSignature != 0xFEEF04BD) return {};

		char out[64] = {};
		std::snprintf(
			out,
			sizeof(out),
			"v%u.%u.%u",
			HIWORD(info->dwProductVersionMS),
			LOWORD(info->dwProductVersionMS),
			HIWORD(info->dwProductVersionLS));
		return out;
	}

	void RefreshAssets(App &app, const fs::path &exeDir)
	{
		static const AssetRow kAssets[] = {
			{"WinDisk 驱动", L"WinDisk_x64.sys"},
			{"KDU（关签名强制）", L"kdu.exe"},
			{"KDU provider 库", L"drv64.dll"},
			{"目标名单", L"targets.txt"},
			{"ntfsfix", L"tools\\ntfsfix.exe"},
			{"ntfscp", L"tools\\ntfscp.exe"},
			{"ntfs-3g-cli", L"tools\\ntfs-3g-cli.exe"},
			{"mkntfs", L"tools\\mkntfs.exe"},
		};
		app.assets.clear();
		for (const auto &a: kAssets)
		{
			AssetRow row = a;
			row.present  = icemelt::PathIsRegularFile(exeDir / a.rel);
			app.assets.push_back(row);
		}
	}

	// 与 LoadTargets 相同的搜索顺序（<exeDir>/targets.txt → <exeDir>/config/targets.txt）；
	// 返回空表示两个都没有，实际用的是编译内置名单。
	fs::path ResolveTargetsFile(const fs::path &exeDir)
	{
		const fs::path first = exeDir / L"targets.txt";
		if (icemelt::PathIsRegularFile(first)) return first;
		const fs::path second = exeDir / L"config" / L"targets.txt";
		if (icemelt::PathIsRegularFile(second)) return second;
		return {};
	}

	FileStamp StatFile(const fs::path &p)
	{
		FileStamp st;
		if (p.empty()) return st;
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return st;
		st.valid = true;
		st.size  = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
		st.mtime = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
				fad.ftLastWriteTime.dwLowDateTime;
		return st;
	}

	// 重新读名单文件并重解析。announce=true 时把来源写进日志（启动/重载/手动刷新用）。
	void RefreshTargets(App &app, const fs::path &exeDir, bool announce)
	{
		const auto                 targets = icemelt::LoadTargets(exeDir);
		const auto                 names   = icemelt::TargetNames(targets);
		const icemelt::ProbeReport probe   = icemelt::ProbeTargets(names);

		app.targetsFile  = ResolveTargetsFile(exeDir);
		app.targetsStamp = StatFile(app.targetsFile);

		app.targets.clear();
		app.targets.reserve(targets.size());
		for (const auto &t: targets)
		{
			TargetRowUI row;
			row.name  = icemelt::Narrow(t.name);
			row.label = icemelt::Narrow(t.label);

			std::vector<std::string> where;
			for (const auto &f: probe.filters)
			{
				for (const auto &item: f.items)
				{
					if (_wcsicmp(item.c_str(), t.name.c_str()) == 0)
						where.push_back(
							"Class\\" + icemelt::Narrow(f.classGuid) + "\\" +
							icemelt::Narrow(f.valueName));
				}
			}
			for (const auto &key: probe.existingServiceKeys)
			{
				if (_wcsicmp(key.c_str(), t.name.c_str()) == 0)
					where.push_back("Services\\" + icemelt::Narrow(key));
			}
			row.present = !where.empty();
			if (where.empty())
			{
				row.status = "not present";
			}
			else
			{
				row.status = "PRESENT: " + where[0];
				for (size_t i = 1; i < where.size(); ++i) row.status += ", " + where[i];
			}
			app.targets.push_back(std::move(row));
		}

		if (announce)
		{
			if (app.targetsFile.empty())
			{
				UiLog(app, "[!] 目标名单: 未找到 targets.txt，使用内置默认名单");
			}
			else
			{
				UiLog(app, "[*] 目标名单已加载: " + icemelt::Narrow(app.targetsFile.wstring()));
			}
		}
	}

	// 名单文件一变就重载（0.5s 一次的取证级轮询：大小 + mtime）。监视的是"当前生效"的
	// 路径：被删/被建/被改都会让戳改变，重载后 ResolveTargetsFile 再决定用哪一个。
	void TickTargetsReload(App &app, double now)
	{
		if (now < app.nextTargetsCheck) return;
		app.nextTargetsCheck    = now + 0.5;
		const fs::path  exeDir  = icemelt::ExeDir();
		const FileStamp current = StatFile(ResolveTargetsFile(exeDir));
		if (current != app.targetsStamp)
		{
			RefreshTargets(app, exeDir, true);
		}
	}

	// ---- 日志管线 ---------------------------------------------------------------

	void UiLog(App &app, const std::string &line)
	{
		app.lines.push_back(line);
		if (app.lines.size() > 5000)
			app.lines.erase(
				app.lines.begin(),
				app.lines.begin() + 1000);
		app.follow = true;
	}

	void DrainSharedLines(App &app)
	{
		std::lock_guard<std::mutex> guard(app.shared.mu);
		const size_t                extra = app.shared.totalLines - app.seen;
		const size_t                n     = extra < app.shared.lines.size()
			                 ? extra
			                 : app.shared.lines.size();
		for (size_t i = app.shared.lines.size() - n; i < app.shared.lines.size(); ++i)
			app.lines.push_back(app.shared.lines[i]);
		app.seen = app.shared.totalLines;
		if (app.lines.size() > 5000)
			app.lines.erase(
				app.lines.begin(),
				app.lines.begin() + 1000);
	}

	// 日志终端行着色：语法高亮，清晰易读，不刺眼
	ImU32 LineColor(const std::string &s)
	{
		if (s.rfind("[x]", 0) == 0 || s.find("FAIL") != std::string::npos || s.find("error") != std::string::npos)
			return IM_COL32(248, 113, 113, 255); // DangerRed
		if (s.rfind("[!]", 0) == 0 || s.find("WARN") != std::string::npos)
			return IM_COL32(251, 191, 36, 255);  // WarningAmber
		if (s.rfind("[+]", 0) == 0 || s.find("PASS") != std::string::npos || s.find("OK") != std::string::npos)
			return IM_COL32(52, 211, 153, 255);  // SuccessGreen
		if (s.rfind("[*]", 0) == 0)
			return IM_COL32(56, 189, 248, 255);  // AccentCyan
		return IM_COL32(220, 225, 235, 255);     // Standard text
	}

	// ---- 任务启停 ---------------------------------------------------------------

	void RunJob(Shared &sh, fs::path exeDir, JobKind kind)
	{
		icemelt::MeltOptions opt;
		opt.exeDir         = std::move(exeDir);
		opt.winDiskSysPath = opt.exeDir / L"WinDisk_x64.sys";
		opt.dryRun         = (kind == JobKind::DryRun);

		const icemelt::MeltLogger logger = [&sh](const std::wstring &w)
		{
			std::lock_guard<std::mutex> guard(sh.mu);
			sh.lines.push_back(icemelt::Narrow(w));
			if (sh.lines.size() > 4000) sh.lines.pop_front();
			++sh.totalLines;
		};

		if (kind == JobKind::Melt)
		{
			opt.confirm = [&sh](const icemelt::MeltResult &pending) -> bool
			{
				std::unique_lock<std::mutex> lock(sh.mu);
				sh.pending.clear();
				for (const auto &item: pending.pending)
					sh.pending.push_back(
						icemelt::Narrow(item));
				sh.confirmAnswered  = false;
				sh.confirmAnswer    = false;
				sh.confirmRequested = true;
				sh.cv.notify_all();
				sh.cv.wait(
					lock,
					[&sh]
					{
						return sh.confirmAnswered;
					});
				const bool answer   = sh.confirmAnswer;
				sh.confirmRequested = false;
				return answer;
			};
		}

		icemelt::MeltResult result;
		if (kind == JobKind::Preflight)
		{
			result = icemelt::RunPreflightScan(opt, logger);
		}
		else
		{
			result = icemelt::RunMelt(opt, logger);
		}

		std::lock_guard<std::mutex> guard(sh.mu);
		sh.result = std::move(result);
		sh.done   = true;
	}

	void StartJob(App &app, JobKind kind)
	{
		if (app.running) return;
		app.kind          = kind;
		app.running       = true;
		app.lastOk        = false;
		app.lastWroteDisk = false;
		app.lines.clear();
		app.seen   = 0;
		app.follow = true;

		{
			std::lock_guard<std::mutex> guard(app.shared.mu);
			app.shared.lines.clear();
			app.shared.totalLines       = 0;
			app.shared.confirmRequested = false;
			app.shared.confirmAnswered  = false;
			app.shared.confirmAnswer    = false;
			app.shared.pending.clear();
			app.shared.done   = false;
			app.shared.result = icemelt::MeltResult{};
		}

		app.worker = std::thread(RunJob, std::ref(app.shared), icemelt::ExeDir(), kind);
	}

	void PollWorker(App &app)
	{
		if (!app.running) return;

		// 抽取日志
		DrainSharedLines(app);

		// 检查 confirm
		{
			std::lock_guard<std::mutex> guard(app.shared.mu);
			if (app.shared.confirmRequested && !app.confirmOpen)
			{
				app.confirmOpen  = true;
				app.confirmItems = app.shared.pending;
				std::memset(app.confirmBuf, 0, sizeof(app.confirmBuf));
			}
		}

		// 检查完成
		{
			std::lock_guard<std::mutex> guard(app.shared.mu);
			if (app.shared.done)
			{
				app.running       = false;
				app.lastKind      = app.kind;
				app.kind          = JobKind::None;
				app.lastOk        = app.shared.result.ok;
				app.lastWroteDisk = app.shared.result.wroteDisk;
				if (app.worker.joinable()) app.worker.join();
			}
		}
	}

	// ---- 外观：Catppuccin Mocha + 触摸尺度 --------------------------------------
	//
	// 调色板取 Catppuccin Mocha。触摸尺度：ImGui 的命中盒由 FramePadding 决定
	// （按钮/复选框/输入框的行高 = 字号 + 2*FramePadding.y），因此按"手指"而非
	// "鼠标"定基准值，把每个控件撑到 ~44px 触控目标以上；TouchExtraPadding 再
	// 给相邻控件留防误触间隔；滚动条加宽到可直接拖。
	//
	// 注意顺序：先设基准值再 ScaleAllSizes(dpiScale)，否则高 DPI 下两套缩放打架。

	ImVec4 Rgb(unsigned rgb, float a = 1.0f)
	{
		return {
			((rgb >> 16) & 0xFF) / 255.0f,
			((rgb >> 8) & 0xFF) / 255.0f,
			((rgb & 0xFF) / 255.0f),
			a
		};
	}

	// 现代工程暗色调色板（精致干练、深炭黑冷灰底色、避免蓝紫渐变）：
	// 主基调：深炭黑底 + 冷灰面板 + 纯白/浅灰文字 + 森林绿/琥珀黄/珊瑚红状态指示 + 冰青微点缀
	namespace palette
	{
		// 背景与分层（深冷炭灰系，层次分明且纯净）
		constexpr unsigned BgDeep       = 0x101216; // 窗口深底
		constexpr unsigned BgSurface    = 0x161920; // 卡片与面板底色
		constexpr unsigned BgCard       = 0x1B1F27; // 凸起卡片底色
		constexpr unsigned BgHeader     = 0x202530; // 顶栏与表头底色
		constexpr unsigned BgInput      = 0x13151B; // 输入框 / 表格底色
		constexpr unsigned BgHover      = 0x282E3C; // 悬停态底色
		constexpr unsigned BgActive     = 0x323A4B; // 激活态底色

		// 边框与分割线（低反差柔和线条）
		constexpr unsigned BorderSubtle = 0x242935; // 弱边框
		constexpr unsigned BorderLight  = 0x333A4B; // 较显眼控件边框
		constexpr unsigned BorderFocus  = 0x4D5870; // 聚焦或高亮线

		// 文字层级
		constexpr unsigned TextMain     = 0xF0F2F5; // 主要文本：纯白略微偏暖，清晰不刺眼
		constexpr unsigned TextMuted    = 0x9DA5B4; // 次级文本：柔和冷灰
		constexpr unsigned TextDark     = 0x606877; // 辅助文本/注释

		// 语义功能色（纯净柔和，不刺目，杜绝蓝紫渐变高饱和度）
		constexpr unsigned AccentCyan   = 0x38BDF8; // 冰青冷点缀色（清爽极客感）
		constexpr unsigned SuccessGreen = 0x34D399; // 成功/就绪状态绿
		constexpr unsigned WarningAmber = 0xFBBF24; // 警告/提示琥珀黄
		constexpr unsigned DangerRed    = 0xF87171; // 危险/失败珊瑚红
		constexpr unsigned NeutralPill  = 0x1F242E; // 胶囊底色
	} // namespace palette

	void ApplyModernDarkTheme(float dpiScale)
	{
		ImGui::StyleColorsDark();
		ImGuiStyle &s = ImGui::GetStyle();

		// 精致干练的桌面尺寸基准：告别臃肿的 44px 工控触摸尺度
		s.WindowPadding     = ImVec2(16.0f, 16.0f);
		s.FramePadding      = ImVec2(10.0f, 6.0f);  // 标准桌面高度 ≈ 字号 + 12px
		s.ItemSpacing       = ImVec2(10.0f, 8.0f);
		s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
		s.CellPadding       = ImVec2(8.0f, 5.0f);
		s.TouchExtraPadding = ImVec2(0.0f, 0.0f);   // 消除多余的防误触边距
		s.ScrollbarSize     = 10.0f;
		s.GrabMinSize       = 10.0f;

		// 统一现代微圆角（4px ~ 6px）
		s.WindowRounding    = 0.0f; // 顶层全屏无标题窗口不倒角
		s.ChildRounding     = 6.0f;
		s.FrameRounding     = 4.0f;
		s.PopupRounding     = 6.0f;
		s.ScrollbarRounding = 5.0f;
		s.GrabRounding      = 4.0f;
		s.TabRounding       = 4.0f;

		// 边框粗细与渲染
		s.WindowBorderSize  = 0.0f;
		s.ChildBorderSize   = 1.0f;
		s.FrameBorderSize   = 1.0f;
		s.PopupBorderSize   = 1.0f;

		// 色彩映射
		ImVec4 *c                             = s.Colors;
		c[ImGuiCol_Text]                      = Rgb(palette::TextMain);
		c[ImGuiCol_TextDisabled]              = Rgb(palette::TextDark);
		c[ImGuiCol_WindowBg]                  = Rgb(palette::BgDeep);
		c[ImGuiCol_ChildBg]                   = Rgb(palette::BgSurface);
		c[ImGuiCol_PopupBg]                   = Rgb(palette::BgCard);
		c[ImGuiCol_Border]                    = Rgb(palette::BorderSubtle);
		c[ImGuiCol_BorderShadow]              = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_FrameBg]                   = Rgb(palette::BgInput);
		c[ImGuiCol_FrameBgHovered]            = Rgb(palette::BgHover);
		c[ImGuiCol_FrameBgActive]             = Rgb(palette::BgActive);
		c[ImGuiCol_TitleBg]                   = Rgb(palette::BgHeader);
		c[ImGuiCol_TitleBgActive]             = Rgb(palette::BgHeader);
		c[ImGuiCol_TitleBgCollapsed]          = Rgb(palette::BgHeader);
		c[ImGuiCol_MenuBarBg]                 = Rgb(palette::BgHeader);
		c[ImGuiCol_ScrollbarBg]               = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ScrollbarGrab]             = Rgb(palette::BorderLight);
		c[ImGuiCol_ScrollbarGrabHovered]      = Rgb(palette::BorderFocus);
		c[ImGuiCol_ScrollbarGrabActive]       = Rgb(palette::TextMuted);
		c[ImGuiCol_CheckMark]                 = Rgb(palette::AccentCyan);
		c[ImGuiCol_CheckboxSelectedBg]        = Rgb(palette::BgInput);
		c[ImGuiCol_SliderGrab]                = Rgb(palette::AccentCyan);
		c[ImGuiCol_SliderGrabActive]          = Rgb(palette::AccentCyan);
		c[ImGuiCol_Button]                    = Rgb(palette::BgCard);
		c[ImGuiCol_ButtonHovered]             = Rgb(palette::BgHover);
		c[ImGuiCol_ButtonActive]              = Rgb(palette::BgActive);
		c[ImGuiCol_Header]                    = Rgb(palette::BgHover);
		c[ImGuiCol_HeaderHovered]             = Rgb(palette::BgActive);
		c[ImGuiCol_HeaderActive]              = Rgb(palette::BorderFocus);
		c[ImGuiCol_Separator]                 = Rgb(palette::BorderSubtle);
		c[ImGuiCol_SeparatorHovered]          = Rgb(palette::BorderLight);
		c[ImGuiCol_SeparatorActive]           = Rgb(palette::BorderFocus);
		c[ImGuiCol_ResizeGrip]                = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered]         = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripActive]          = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TabHovered]                = Rgb(palette::BgHover);
		c[ImGuiCol_Tab]                       = Rgb(palette::BgSurface);
		c[ImGuiCol_TabSelected]               = Rgb(palette::BgHeader);
		c[ImGuiCol_TabSelectedOverline]       = Rgb(palette::AccentCyan);
		c[ImGuiCol_TabDimmed]                 = Rgb(palette::BgSurface);
		c[ImGuiCol_TabDimmedSelected]         = Rgb(palette::BgHeader);
		c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(palette::BorderLight);
		c[ImGuiCol_InputTextCursor]           = Rgb(palette::AccentCyan);
		c[ImGuiCol_TableHeaderBg]             = Rgb(palette::BgHeader);
		c[ImGuiCol_TableBorderStrong]         = Rgb(palette::BorderSubtle);
		c[ImGuiCol_TableBorderLight]          = Rgb(palette::BorderSubtle, 0.4f);
		c[ImGuiCol_TableRowBg]                = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TableRowBgAlt]             = Rgb(palette::BgDeep, 0.35f);
		c[ImGuiCol_TextLink]                  = Rgb(palette::AccentCyan);
		c[ImGuiCol_TextSelectedBg]            = Rgb(palette::AccentCyan, 0.25f);
		c[ImGuiCol_NavCursor]                 = Rgb(palette::AccentCyan);
		c[ImGuiCol_ModalWindowDimBg]          = ImVec4(0.04f, 0.05f, 0.07f, 0.75f);

		s.ScaleAllSizes(dpiScale);
	}
	// ---- 精致干练的 UI 小型组件库 ----------------------------------------------

	// 状态胶囊（Badge）：圆角纯色小底色 + 语义文字
	void RenderBadge(const char *text, unsigned fgColor, unsigned bgColor, float dpiScale)
	{
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 padding(7.0f * dpiScale, 3.0f * dpiScale);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 min = pos;
		const ImVec2 max = ImVec2(pos.x + textSize.x + padding.x * 2.0f, pos.y + textSize.y + padding.y * 2.0f);

		const ImU32 bgU32 = (bgColor & 0xFFFFFF00) ? bgColor : ((bgColor << 8) | 0xFF);
		const ImU32 fgU32 = (fgColor & 0xFFFFFF00) ? fgColor : ((fgColor << 8) | 0xFF);

		ImDrawList *drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, bgU32, 4.0f * dpiScale);
		drawList->AddText(ImVec2(min.x + padding.x, min.y + padding.y), fgU32, text);

		ImGui::Dummy(ImVec2(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f));
	}

	// 状态圆点指示灯（带柔和外光晕感）
	void RenderStatusDot(unsigned color, float radius = 3.5f)
	{
		const float lineHeight = ImGui::GetTextLineHeight();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 center(pos.x + radius + 2.0f, pos.y + lineHeight * 0.5f);

		const ImU32 colU32 = (color & 0xFFFFFF00) ? color : ((color << 8) | 0xFF);
		const ImU32 haloU32 = (colU32 & 0x00FFFFFF) | 0x33000000;

		ImDrawList *drawList = ImGui::GetWindowDrawList();
		drawList->AddCircleFilled(center, radius + 2.0f, haloU32);
		drawList->AddCircleFilled(center, radius, colU32);

		ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, lineHeight));
	}

	// 区块分节小标题（精炼优雅，不再是一整行刺目的彩色）
	void RenderSectionHeader(const char *title, const char *subtext = nullptr)
	{
		ImGui::Spacing();
		ImGui::TextColored(Rgb(palette::TextMain), "%s", title);
		if (subtext != nullptr && subtext[0] != '\0')
		{
			ImGui::SameLine();
			ImGui::TextColored(Rgb(palette::TextDark), "· %s", subtext);
		}
		ImGui::Spacing();
	}

	// ---- D3D9 设备管理 ----------------------------------------------------------

	static LPDIRECT3D9           g_pD3D        = nullptr;
	static LPDIRECT3DDEVICE9     g_pd3dDevice  = nullptr;
	static bool                  g_DeviceLost  = false;
	static UINT                  g_ResizeWidth = 0, g_ResizeHeight = 0;
	static D3DPRESENT_PARAMETERS g_d3dpp       = {};

	bool CreateDeviceD3D(HWND hWnd)
	{
		if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr) return false;

		ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
		g_d3dpp.Windowed               = TRUE;
		g_d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
		g_d3dpp.BackBufferFormat       = D3DFMT_UNKNOWN;
		g_d3dpp.EnableAutoDepthStencil = TRUE;
		g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
		g_d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
		if (g_pD3D->CreateDevice(
			D3DADAPTER_DEFAULT,
			D3DDEVTYPE_HAL,
			hWnd,
			D3DCREATE_HARDWARE_VERTEXPROCESSING,
			&g_d3dpp,
			&g_pd3dDevice) < 0)
			return false;
		return true;
	}

	void CleanupDeviceD3D()
	{
		if (g_pd3dDevice)
		{
			g_pd3dDevice->Release();
			g_pd3dDevice = nullptr;
		}
		if (g_pD3D)
		{
			g_pD3D->Release();
			g_pD3D = nullptr;
		}
	}

	void ResetDevice()
	{
		ImGui_ImplDX9_InvalidateDeviceObjects();
		HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
		if (hr == D3DERR_INVALIDCALL)
			IM_ASSERT(0);
		ImGui_ImplDX9_CreateDeviceObjects();
	}

	// ---- Win32 消息循环 ---------------------------------------------------------

	LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

		switch (msg)
		{
			case WM_SIZE:
				if (wParam == SIZE_MINIMIZED) return 0;
				g_ResizeWidth  = (UINT) LOWORD(lParam);
				g_ResizeHeight = (UINT) HIWORD(lParam);
				return 0;
			case WM_SYSCOMMAND:
				if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
				break;
			case WM_CLOSE:
				// 运行中禁止关闭窗口（防止 worker 线程持有驱动句柄时进程退出导致蓝屏）
				if (g_app && g_app->running)
				{
					::MessageBoxW(
						hWnd,
						L"任务正在运行，请等待完成后再关闭。",
						L"IceMelt",
						MB_OK | MB_ICONWARNING);
					return 0;
				}
				break;
			case WM_DESTROY:
				::PostQuitMessage(0);
				return 0;
		}
		return ::DefWindowProcW(hWnd, msg, wParam, lParam);
	}

	// ---- ImGui 主界面 -----------------------------------------------------------

	void RenderUI(App &app)
	{
		const ImGuiIO &io = ImGui::GetIO();

		// 名单文件动态加载：文件一变就重载
		TickTargetsReload(app, ImGui::GetTime());

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin(
			"##Main",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoBringToFrontOnFocus);

		// ---- 顶部导航与状态条 (Header & Status Bar) --------------------------------
		{
			// 标题区
			if (app.fontTitle != nullptr) ImGui::PushFont(app.fontTitle, 20.0f);
			ImGui::TextColored(Rgb(palette::TextMain), "IceMelt");
			if (app.fontTitle != nullptr) ImGui::PopFont();

			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(Rgb(palette::TextDark), "%s", app.version.c_str());

			ImGui::SameLine();
			ImGui::TextColored(Rgb(palette::TextMuted), "—— 裸盘级冻结解除工具");

			// 右侧状态胶囊（系统信息与提权状态）
			const float rightMargin = 16.0f * app.dpiScale;
			const char *elevText = app.elevated ? "已提权 (Admin)" : "未提权 (只读)";
			const unsigned elevFg = app.elevated ? palette::SuccessGreen : palette::WarningAmber;
			const unsigned elevBg = app.elevated ? 0x15281E : 0x2A2215;

			const ImVec2 elevSize = ImGui::CalcTextSize(elevText);
			const float badgePad = 12.0f * app.dpiScale;
			const float totalBadgeW = elevSize.x + badgePad + 18.0f * app.dpiScale;

			ImGui::SameLine(ImGui::GetWindowWidth() - totalBadgeW - rightMargin);
			RenderStatusDot(elevFg, 3.5f * app.dpiScale);
			ImGui::SameLine();
			RenderBadge(elevText, elevFg, elevBg, app.dpiScale);

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}

		// ---- 左右分栏布局 -----------------------------------------------------------
		const float paneGap = ImGui::GetStyle().ItemSpacing.x;
		// 稍微将左栏比例定在 48%，右栏 52%，给日志和操作留出舒适空间
		const float leftW = (ImGui::GetContentRegionAvail().x - paneGap) * 0.48f;

		// ==== 左侧面板：运行期资产与目标探测 ==========================================
		ImGui::BeginChild("LeftPane", ImVec2(leftW, 0), true);
		{
			// ---- 1. 系统与资产概览 ----
			RenderSectionHeader("环境与资产", "本地运行时组件清点");

			// 系统信息微卡片
			ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(palette::BgInput));
			ImGui::BeginChild("SysInfoCard", ImVec2(0, 58.0f * app.dpiScale), true);
			{
				ImGui::TextColored(Rgb(palette::TextDark), "操作系统: ");
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextMain), "%s", app.osVersion.c_str());

				ImGui::TextColored(Rgb(palette::TextDark), "程序目录: ");
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextMuted), "%s", app.exeDirUtf8.c_str());
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();

			ImGui::Spacing();

			// 资产表格（双列紧凑排布）
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * app.dpiScale, 4.0f * app.dpiScale));
			{
				constexpr int kPerRow = 2;
				const int rows = (static_cast<int>(app.assets.size()) + kPerRow - 1) / kPerRow;
				if (ImGui::BeginTable("##Assets", kPerRow * 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Item0", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Status0", ImGuiTableColumnFlags_WidthFixed, 60.0f * app.dpiScale);
					ImGui::TableSetupColumn("Item1", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Status1", ImGuiTableColumnFlags_WidthFixed, 60.0f * app.dpiScale);

					for (int r = 0; r < rows; ++r)
					{
						ImGui::TableNextRow();
						for (int c = 0; c < kPerRow; ++c)
						{
							const int idx = r * kPerRow + c;
							if (idx >= static_cast<int>(app.assets.size()))
							{
								ImGui::TableNextColumn();
								ImGui::TableNextColumn();
								continue;
							}
							const auto &a = app.assets[idx];
							ImGui::TableNextColumn();
							ImGui::AlignTextToFramePadding();
							ImGui::TextColored(Rgb(palette::TextMain), "%s", a.label);
							ImGui::TableNextColumn();
							if (a.present)
							{
								RenderBadge("就绪", palette::SuccessGreen, 0x162A1F, app.dpiScale);
							}
							else
							{
								RenderBadge("缺失", palette::DangerRed, 0x2C181B, app.dpiScale);
							}
						}
					}
					ImGui::EndTable();
				}
			}
			ImGui::PopStyleVar();

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			// ---- 2. 目标探测 ----
			RenderSectionHeader("目标探测", "驱动与注册表服务项");

			if (ImGui::Button("重新加载名单"))
			{
				RefreshTargets(app, icemelt::ExeDir(), true);
			}
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			if (app.targetsFile.empty())
			{
				RenderBadge("内置默认 (未找到 targets.txt)", palette::TextDark, palette::NeutralPill, app.dpiScale);
			}
			else
			{
				const std::string fn = icemelt::Narrow(app.targetsFile.filename().wstring());
				RenderBadge(fn.c_str(), palette::AccentCyan, palette::NeutralPill, app.dpiScale);
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", icemelt::Narrow(app.targetsFile.wstring()).c_str());
				}
			}

			ImGui::Spacing();

			// 目标清单自适应高度表格
			static float slack = 0.0f;
			const float applied = slack;
			const float availY = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y;
			const float minH = 160.0f * app.dpiScale;
			const bool clamped = (availY - applied) < minH;
			const float targetsH = clamped ? minH : (availY - applied);

			if (ImGui::BeginTable(
				"##Targets",
				3,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
				ImVec2(0, targetsH)))
			{
				ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 90.0f * app.dpiScale);
				ImGui::TableSetupColumn("标签", ImGuiTableColumnFlags_WidthFixed, 100.0f * app.dpiScale);
				ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();

				for (const auto &t: app.targets)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextColored(Rgb(palette::TextMain), "%s", t.name.c_str());

					ImGui::TableNextColumn();
					ImGui::TextColored(Rgb(palette::TextMuted), "%s", t.label.c_str());

					ImGui::TableNextColumn();
					if (t.present)
					{
						RenderBadge("存在", palette::WarningAmber, 0x2A2215, app.dpiScale);
						ImGui::SameLine();
						ImGui::TextColored(Rgb(palette::WarningAmber), "%s", t.status.c_str());
					}
					else
					{
						ImGui::TextColored(Rgb(palette::TextDark), "未探测到");
					}
				}
				ImGui::EndTable();
			}
			if (!clamped) slack = applied + ImGui::GetScrollMaxY();
		}
		ImGui::EndChild();

		ImGui::SameLine();

		// ==== 右侧面板：操作与控制台日志 ============================================
		ImGui::BeginChild("RightPane", ImVec2(0, 0), true);
		{
			// ---- 1. 任务动作区域 ----
			RenderSectionHeader("操作控制", "解冻管线与任务分发");

			const bool canAct = app.elevated && !app.running;
			const float actionBtnH = 36.0f * app.dpiScale; // 干练现代的标准操作按钮高度

			ImGui::BeginDisabled(!canAct);

			// 常规动作：预检扫描 与 只读预演 并排
			const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

			if (ImGui::Button("预检扫描 (--preflight)", ImVec2(halfW, actionBtnH)))
			{
				StartJob(app, JobKind::Preflight);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("装载驱动并运行四层探针读写校验（请务必在已创建快照的虚拟机中测试）");
			}

			ImGui::SameLine();

			if (ImGui::Button("只读预演 (--dry-run)", ImVec2(halfW, actionBtnH)))
			{
				StartJob(app, JobKind::DryRun);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("执行全链路预演逻辑，不产生任何裸盘写入");
			}

			ImGui::Spacing();

			// 危险操作：MELT
			const bool isMeltActive = canAct;
			if (isMeltActive)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, Rgb(0x421D24));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0x5E2833));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x782F3E));
				ImGui::PushStyleColor(ImGuiCol_Text, Rgb(palette::DangerRed));
				ImGui::PushStyleColor(ImGuiCol_Border, Rgb(palette::DangerRed, 0.4f));
			}

			if (ImGui::Button("执行 MELT 离线解除 (--melt)", ImVec2(-FLT_MIN, 40.0f * app.dpiScale)))
			{
				StartJob(app, JobKind::Melt);
			}

			if (isMeltActive)
			{
				ImGui::PopStyleColor(5);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("写回离线 SYSTEM Hive 并置 dirty 标记，触发系统自修复解冻流程");
			}

			ImGui::EndDisabled();

			ImGui::Spacing();

			// 运行状态与结果条
			ImGui::AlignTextToFramePadding();
			if (app.running)
			{
				RenderStatusDot(palette::WarningAmber, 3.5f * app.dpiScale);
				ImGui::SameLine();
				RenderBadge("执行中", palette::WarningAmber, 0x2A2215, app.dpiScale);
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextMain), "正在运行: %s", JobName(app.kind));
			}
			else if (app.lastKind != JobKind::None)
			{
				if (app.lastOk)
				{
					RenderStatusDot(palette::SuccessGreen, 3.5f * app.dpiScale);
					ImGui::SameLine();
					RenderBadge("成功", palette::SuccessGreen, 0x162A1F, app.dpiScale);
				}
				else
				{
					RenderStatusDot(palette::DangerRed, 3.5f * app.dpiScale);
					ImGui::SameLine();
					RenderBadge("失败", palette::DangerRed, 0x2C181B, app.dpiScale);
				}
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextMain), "%s 完成", JobName(app.lastKind));

				if (app.lastWroteDisk)
				{
					ImGui::SameLine();
					RenderBadge("已写入裸盘", palette::DangerRed, 0x36161B, app.dpiScale);
				}
			}
			else
			{
				RenderStatusDot(palette::TextDark, 3.5f * app.dpiScale);
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextDark), "就绪，等待下发任务");
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			// ---- 2. 日志控制台终端 ----
			RenderSectionHeader("输出终端", "实时执行日志与取证输出");

			ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f - 110.0f * app.dpiScale);
			ImGui::Checkbox("自动滚动", &app.follow);
			ImGui::SameLine();
			if (ImGui::Button("清空"))
			{
				app.lines.clear();
			}

			ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(palette::BgDeep));
			ImGui::BeginChild(
				"##Log",
				ImVec2(0, 0),
				true,
				ImGuiWindowFlags_HorizontalScrollbar);
			{
				if (app.fontMono != nullptr) ImGui::PushFont(app.fontMono, 13.5f);

				for (const auto &line: app.lines)
				{
					ImGui::TextColored(
						ImGui::ColorConvertU32ToFloat4(LineColor(line)),
						"%s",
						line.c_str());
				}

				if (app.fontMono != nullptr) ImGui::PopFont();

				if (app.follow && !app.lines.empty())
				{
					ImGui::SetScrollHereY(1.0f);
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();

		ImGui::End();

		// confirm 模态框（统一采用现代干练的弹窗规范）
		if (app.confirmOpen)
		{
			ImGui::OpenPopup("确认写入");
			if (ImGui::BeginPopupModal(
				"确认写入",
				nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
			{
				RenderSectionHeader("危险操作确认", "请仔细核对即将执行的裸盘写入项");
				ImGui::TextColored(Rgb(palette::WarningAmber), "此操作不可逆！以下项目将被写入目标磁盘：");
				ImGui::Spacing();

				ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(palette::BgInput));
				ImGui::BeginChild("ConfirmItemsBox", ImVec2(460.0f * app.dpiScale, 120.0f * app.dpiScale), true);
				for (const auto &item: app.confirmItems)
				{
					ImGui::BulletText("%s", item.c_str());
				}
				ImGui::EndChild();
				ImGui::PopStyleColor();

				ImGui::Spacing();
				ImGui::TextColored(Rgb(palette::TextMuted), "请输入 ");
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::DangerRed), "MELT");
				ImGui::SameLine();
				ImGui::TextColored(Rgb(palette::TextMuted), " 并确认以继续：");
				ImGui::Spacing();

				ImGui::SetNextItemWidth(460.0f * app.dpiScale);
				ImGui::InputText(
					"##ConfirmInput",
					app.confirmBuf,
					sizeof(app.confirmBuf));
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();

				const bool match = (std::strcmp(app.confirmBuf, "MELT") == 0);
				const float modalBtnW = 120.0f * app.dpiScale;
				const float modalBtnH = 34.0f * app.dpiScale;

				if (match)
				{
					ImGui::PushStyleColor(ImGuiCol_Button, Rgb(0x421D24));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0x5E2833));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x782F3E));
					ImGui::PushStyleColor(ImGuiCol_Text, Rgb(palette::DangerRed));
				}
				else
				{
					ImGui::BeginDisabled();
				}

				if (ImGui::Button("确认写入", ImVec2(modalBtnW, modalBtnH)))
				{
					std::lock_guard<std::mutex> guard(app.shared.mu);
					app.shared.confirmAnswer   = true;
					app.shared.confirmAnswered = true;
					app.shared.cv.notify_all();
					app.confirmOpen = false;
					ImGui::CloseCurrentPopup();
				}

				if (match)
				{
					ImGui::PopStyleColor(4);
				}
				else
				{
					ImGui::EndDisabled();
				}

				ImGui::SameLine();
				if (ImGui::Button("取消", ImVec2(modalBtnW, modalBtnH)))
				{
					std::lock_guard<std::mutex> guard(app.shared.mu);
					app.shared.confirmAnswer   = false;
					app.shared.confirmAnswered = true;
					app.shared.cv.notify_all();
					app.confirmOpen = false;
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}
		}
	}
} // namespace

// ---- 入口点 -----------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
	// DPI 感知
	ImGui_ImplWin32_EnableDpiAwareness();
	float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(
		::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

	// 创建窗口
	WNDCLASSEXW wc = {
		sizeof(wc),
		CS_CLASSDC,
		WndProc,
		0L,
		0L,
		hInstance,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		L"IceMelt",
		nullptr
	};
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(
		wc.lpszClassName,
		L"IceMelt — 裸盘级冻结解除",
		WS_OVERLAPPEDWINDOW,
		100,
		100,
		(int)(1280 * mainScale),
		(int)(800 * mainScale),
		nullptr,
		nullptr,
		wc.hInstance,
		nullptr);

	// 初始化 D3D9
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	// 初始化 ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	(void) io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	// 布局是固定单窗口，不需要持久化；默认会在**工作目录**写出 imgui.ini
	io.IniFilename = nullptr;

	ApplyModernDarkTheme(mainScale);
	ImGui::GetStyle().FontScaleDpi = mainScale;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX9_Init(g_pd3dDevice);

	// 字体配置：
	// 1. 默认常规 UI 字体（微软雅黑 / 等，15px，紧凑干练）
	// 2. 标题字体（18px）
	// 3. 极客等宽终端字体（Consolas，用于日志区）
	ImFont *fontRegular = nullptr;
	ImFont *fontTitle   = nullptr;
	ImFont *fontMono    = nullptr;
	{
		wchar_t winDir[MAX_PATH] = {};
		::GetWindowsDirectoryW(winDir, MAX_PATH);
		const std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";

		const wchar_t *kCandidates[] = {
			L"msyh.ttc",
			L"msyh.ttf",
			L"Deng.ttf",
			L"simhei.ttf",
			L"simsun.ttc"
		};

		std::string foundUiFont;
		for (const wchar_t *name: kCandidates)
		{
			const fs::path candidate = fs::path(fontsDir) / name;
			if (!icemelt::PathIsRegularFile(candidate)) continue;
			foundUiFont = icemelt::Narrow(candidate.wstring());
			break;
		}

		if (!foundUiFont.empty())
		{
			fontRegular = io.Fonts->AddFontFromFileTTF(foundUiFont.c_str(), 15.0f);
			fontTitle   = io.Fonts->AddFontFromFileTTF(foundUiFont.c_str(), 19.0f);
		}

		const fs::path consolasPath = fs::path(fontsDir) / L"consola.ttf";
		if (icemelt::PathIsRegularFile(consolasPath) && !foundUiFont.empty())
		{
			fontMono = io.Fonts->AddFontFromFileTTF(
				icemelt::Narrow(consolasPath.wstring()).c_str(),
				14.0f);
			ImFontConfig mergeCfg;
			mergeCfg.MergeMode = true;
			io.Fonts->AddFontFromFileTTF(
				foundUiFont.c_str(),
				14.0f,
				&mergeCfg);
		}
		else if (fontRegular != nullptr)
		{
			fontMono = fontRegular;
		}
	}

	// 初始化 App 状态
	App app;
	app.hwnd              = hwnd;
	app.dpiScale          = mainScale;
	app.fontMono          = fontMono;
	app.fontTitle         = fontTitle;
	app.elevated          = icemelt::IsProcessElevated();
	app.version           = VersionString();
	app.osVersion         = WindowsVersionString();
	const fs::path exeDir = icemelt::ExeDir();
	app.exeDirUtf8        = icemelt::Narrow(exeDir.wstring());
	RefreshAssets(app, exeDir);
	g_app = &app;

	UiLog(app, "[*] IceMelt GUI initialized");
	if (!app.elevated)
	{
		UiLog(app, "[!] NOT elevated: all action buttons are disabled");
	}
	RefreshTargets(app, exeDir, true);

	// 主循环
	bool done = false;
	while (!done)
	{
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;

		// 处理设备丢失
		if (g_DeviceLost)
		{
			HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
			if (hr == D3DERR_DEVICELOST)
			{
				::Sleep(10);
				continue;
			}
			if (hr == D3DERR_DEVICENOTRESET) ResetDevice();
			g_DeviceLost = false;
		}

		// 处理窗口 resize
		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			g_d3dpp.BackBufferWidth  = g_ResizeWidth;
			g_d3dpp.BackBufferHeight = g_ResizeHeight;
			g_ResizeWidth            = g_ResizeHeight = 0;
			ResetDevice();
		}

		// 轮询 worker 线程
		PollWorker(app);

		// 渲染帧
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		RenderUI(app);

		ImGui::EndFrame();
		g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
		g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		g_pd3dDevice->Clear(
			0,
			nullptr,
			D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
			D3DCOLOR_RGBA(18, 20, 24, 255),
			1.0f,
			0);
		if (g_pd3dDevice->BeginScene() >= 0)
		{
			ImGui::Render();
			ImDrawData *drawData = ImGui::GetDrawData();
			if (drawData != nullptr)
			{
				ImGui_ImplDX9_RenderDrawData(drawData);
			}
			g_pd3dDevice->EndScene();
		}
		HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
		if (result == D3DERR_DEVICELOST) g_DeviceLost = true;
	}

	// 清理
	if (app.worker.joinable()) app.worker.join();

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}
