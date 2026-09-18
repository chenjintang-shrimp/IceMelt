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

// ntfs-3g-cli "handle:..." setdirty —— 在 $Volume 里置 VOLUME_IS_DIRTY。
//
// 这就是 chkdsk /f 对在用卷做的事：置上标记后，启动时 autochk 会检查并修复这个卷（默认的
// BootExecute 是 "autocheck autochk *"，它只检查带这个标记的卷）。我们绕开文件系统直接改过盘，
// 需要这一道兜底。
//
// 走 libntfs-3g 的 ntfs_volume_write_flags()，写经 handle: → WinDisk 驱动 → 基线磁盘；
// 因此卷被还原类软件冻结时同样生效 —— 而 FSCTL_MARK_VOLUME_DIRTY 走的是挂载中的文件系统，
// 那种写会被截在增量区、到不了基线。
bool SetNtfsVolumeDirty(HANDLE rawDevice, const VolumeInfo& vol, int& exitCode,
                        std::wstring& output, std::wstring& error);

// 把 ntfs-3g 看到的文件开头若干字节抄到本地文件（ntfs-3g-cli ... readhead）。
//
// 为什么需要它：活动 SYSTEM hive 被内核独占持有，按 Win32 路径 CreateFileW 会得到
// ERROR_SHARING_VIOLATION(32) —— 没法用普通 API 打开它校验。而且校验**必须**看 ntfs-3g 的
// 视图：ntfscp 就是按这个视图写盘的，运行中文件系统的视图与它不一致正是"写到别人簇上"的机理。
bool NtfsReadFile(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                  const std::filesystem::path& localOut, uint64_t bytes, int& exitCode,
                  std::wstring& output, std::wstring& error, uint64_t* fileSize = nullptr);

// 解析一个卷内路径到 inode 并报告它（ntfs-3g-cli ... stat:<path>）。
// 返回 true = 该路径存在（out 里是 "size=... mode=..."），false = 不存在/出错（out 里是原因）。
// 用途：读不到时逐层 stat，定位是**哪一层**断的 —— ntfs_pathname_to_inode 逐层查找，
// 任一层缺失都只报 ENOENT，光看最末一层分不清"父目录不存在"还是"文件不存在"。
bool NtfsStatPath(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                  std::wstring& out);

// 列一个卷内目录（ntfs-3g-cli ... list:<path>，只读直接挂载）。每行 "<D|F>\t<name>"。
// 用途：目标名字查不到时，把父目录**实际有什么**摆出来 —— ntfs_pathname_to_inode 只报
// ENOENT，看不出是"索引里根本没这个名字"还是"查找本身有问题"。
bool NtfsListDir(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                 std::wstring& out);

// 回填**卷上真实的名字**（ntfs-3g-cli ... resolve:<path>，大小写不敏感回退）。
// 关键用途：写入用的目标路径必须取自这个结果。ntfscp 在目标查不到时会 ntfs_new_file
// **新建**一个 —— 如果卷上真实名字是 `system` 而我们传 `SYSTEM`，那就会写出一个同目录的
// 新文件（大小写不同的重名），真正的 hive 反而没被改。
bool NtfsResolvePath(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                     std::wstring& canonical, bool* ambiguous = nullptr);

bool NtfsDeleteFile(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                    std::wstring& out);

// 把磁盘上那个 inode 的关键元数据与 runlist 取回来（ntfs-3g-cli ... info:<path>）。
// 分辨"读回一大片零"的成因用：NTFS 读到 initialized_size 之外返回零，runlist 的 hole 也返回零。
bool NtfsAttrInfo(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                  std::wstring& out);

// 把磁盘上那个 inode 的 MFT 记录原始布局/字节取回来（ntfs-3g-cli ... record:<path>）。
// initialized_size 出现"任何合法写入者都产生不了的值"时，用字节本身定案：
// 属性布局、记录序号、$STANDARD_INFORMATION 时间戳是"最后写入者"的笔迹。
bool NtfsRecordDump(HANDLE rawDevice, const VolumeInfo& vol, const std::wstring& ntfsPath,
                    std::wstring& out);

// 只读探测 $LogFile 重启页版本与卷 dirty 位（ntfs-3g-cli ... logstate）。
// RW 挂载在重启页 v2.0 时被 libntfs-3g 无条件拒绝（"Windows 持有缓存元数据"）。
bool NtfsLogState(HANDLE rawDevice, const VolumeInfo& vol, std::wstring& out);

// 组装 "handle:H:OFF:LEN" 参数（供调用方拼命令行与日志）
std::wstring HandleSpec(HANDLE rawDevice, const VolumeInfo& vol);

// Windows 路径 -> NTFS 卷内路径（"C:\a\b" -> "/a/b"）。
// ntfscp 的 dest 只认 '/' 分隔符（libntfs-3g 的 PATH_SEP 就是 '/'），
// 传反斜杠会把整串当成一个文件名。
std::wstring ToNtfsPath(const std::wstring& windowsPath);

}  // namespace secmelt
