// SecMelt —— 外部 ntfs-3g 工具调用与文件物理位置
//
// 命令行形态照抄原作者 DeepFrz/DiskIO.cpp（已验证）：
//   ntfsfix  "handle:<handle>:<volumeOffset>:<volumeLength>"
//   ntfscp -f -v "handle:<handle>:<volumeOffset>:<volumeLength>" "<localSrc>" "<ntfsDest>"
//
// handle: 协议由 third_party/ntfs-3g/libntfs-3g/win32_io.c 的
// ntfs_device_win32_open() 解析（"handle:%zu:%lld:%lld"），它把该句柄当作卷设备使用
// （SetFilePointerEx + ReadFile/WriteFile，偏移 = 卷偏移 + 卷内相对偏移）——
// 也就是 WinDisk 驱动实现的读写语义。句柄必须被子进程继承（见 win_disk.h::Open）。

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "win_disk.h"

namespace secmelt {

// 文件在物理磁盘上的连续区段
struct Extent {
    uint64_t physicalOffset = 0;  // 磁盘绝对字节偏移（已含卷偏移）
    uint64_t length = 0;
};

// 依次在 <exeDir>/tools/<name>、<exeDir>/<name>、PATH 中查找；找不到返回空路径
std::filesystem::path ResolveTool(const std::wstring& exeName);

// 取文件在磁盘上的物理区段。
// 需要 SE_BACKUP_NAME + FILE_FLAG_BACKUP_SEMANTICS：config 目录被 ACL 锁死，
// 普通 open() 对本机 SYSTEM/SYSTEM.LOG1 均返回 Access denied。
bool ListExtentsPhysical(const std::filesystem::path& file, const VolumeInfo& vol,
                         std::vector<Extent>& out, std::wstring& error);

// 运行外部程序，合并捕获 stdout/stderr，回报退出码。
// 句柄继承必须打开（bInheritHandles=TRUE），否则 handle: 协议拿不到卷句柄。
bool RunTool(const std::filesystem::path& exe, const std::wstring& args, int& exitCode,
             std::wstring& output);

// ntfsfix "handle:..."；exitCode 回填
bool NtfsFix(HANDLE rawDevice, const VolumeInfo& vol, int& exitCode, std::wstring& output,
             std::wstring& error);

// ntfscp -f -v "handle:..." <localSrc> <ntfsDest>（卷内路径形如
// "\\Windows\\System32\\config\\SYSTEM"）
bool NtfsCopyIn(HANDLE rawDevice, const VolumeInfo& vol, const std::filesystem::path& localSrc,
                const std::wstring& ntfsDest, int& exitCode, std::wstring& output,
                std::wstring& error);

// 组装 "handle:H:OFF:LEN" 参数（供调用方拼命令行与日志）
std::wstring HandleSpec(HANDLE rawDevice, const VolumeInfo& vol);

// Windows 路径 -> NTFS 卷内路径（"C:\a\b" -> "/a/b"）。
// ntfscp 的 dest 只认 '/' 分隔符（libntfs-3g 的 PATH_SEP 就是 '/'），
// 传反斜杠会把整串当成一个文件名。
std::wstring ToNtfsPath(const std::wstring& windowsPath);

}  // namespace secmelt
