// 卷句柄占用探测：当 FSCTL_LOCK_VOLUME 返回 5 时，回答"是谁握着这个卷"。
//
// 两条写路径在这里汇合的必要性：裸写物理盘上"已挂载文件系统覆盖"的扇区，
// Windows (volmgr/partmgr 的 DASD 守卫) 只有在该卷被锁/卸载后才放行；锁又只在
// 卷上没有其他任何打开句柄时才成功。锁不上时错误只有一个 5，而幕后有两种可能，
// 处置完全不同：
//   1. 某个进程持有打开句柄（资源管理器、杀毒扫描、DG 后台服务、我们的枚举漏关）
//      —— 凶手能找到，关掉重试；
//   2. 卷/文件系统过滤器（典型：DeepFreeeze 的 DFFilter）直接否决锁 —— 锁+卸载
//      会绕过它的 COW，所以它必须拦。这时枚举句柄列不出任何人。
// 本模块就是胺证两者的工具：枚举全系统句柄表（NtQuerySystemInformation 类 64），
// 对每个 File 类型句柄做 0 风险的名字解析（DuplicateHandle + GetFileType 过滤掉
// 管道/套接字，再用 GetFinalPathNameByHandle 取 \Device\HarddiskVolumeN\...），
// 逐个匹配目标卷。不加载驱动、不写盘、只读句柄 —— 可安全在宿主机烟测。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace icemelt
{
	// 找出覆盖物理盘 [diskOffset, diskOffset+length) 的 HarddiskVolumeN。
	// 只打开/探测（GENERIC_READ，问完即关），不锁、不卸载。找到时 deviceName
	// 为 "HarddiskVolume1" 这样的纯设备名（不带 \\Device\ 前缀）。
	// 找不到覆盖卷不算错误（返回 true + 空名）。
	bool FindCoveringVolumeName(
		uint32_t      diskNumber,
		uint64_t      diskOffset,
		uint64_t      length,
		std::wstring &deviceName,
		std::wstring &note);

	struct VolumeHolder
	{
		uint32_t     pid = 0;
		std::wstring processName; // 进程映像全路径；拿不到（受保护进程等）时为空
		std::wstring objectPath;  // \Device\HarddiskVolumeN\...
		uint32_t     access = 0;  // GrantedAccess
		uint32_t     handles = 0; // 同 (pid, path) 的句柄数
	};

	// 列出当前系统里所有打开 name（"HarddiskVolume1"，含其子路径）的句柄及持有进程。
	// note 回填"有多少句柄因进程受保护/CopyHandle 拒绝/路径取不到而被跳过"（零跳过时
	// note 为空）；列出一个都不剩并不代表没人 —— 被过滤器否决的锁在这个表上就是空的，
	// 这正是要区分开的第二种情况。
	bool ListVolumeHolders(
		const std::wstring &     name,
		std::vector<VolumeHolder> &out,
		std::wstring &           note,
		std::wstring &           error);
} // namespace icemelt
