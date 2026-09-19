// SecMelt —— 公共小工具：字符串转换、提权判定、特权开关、路径与时间
//
// 约定：所有面向控制台的文案一律 ASCII。Windows 控制台默认代码页多为 936(GBK)，
// 非 ASCII 文案在部分终端会花屏；宽字符只在 Win32 API 边界与文件内容（名单文件是
// UTF-8）之间转换时使用。

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace secmelt {

// UTF-8 <-> UTF-16
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

// 进程是否以管理员令牌运行（加载内核驱动、写裸盘的硬前提）
bool IsProcessElevated();

// 在当前进程令牌上启用一个特权（如 SE_BACKUP_NAME / SE_SHUTDOWN_NAME）。
// 失败时把 Win32 错误码写进 error 并返回 false。
bool EnablePrivilege(const wchar_t* name, std::wstring& error);

// 可执行文件所在目录
std::filesystem::path ExeDir();

// ---- 路径谓词：绕开 std::filesystem 的"打开句柄"路径 ------------------------
//
// MSVC 的 STL 把 std::filesystem 的文件操作实现为 __std_fs_open_handle()，
// 而它在 14.4x（VS 2026）里**无条件调用 CreateFile2**（Win8+ API），且这段代码
// 是预编译进 libcpmt.lib / msvcprt.lib 的 —— 定义 _WIN32_WINNT=0x0601 也改不了
// （实测：仍是 CreateFile2 导入）。后果是链接了 std::filesystem 的程序在
// Windows 7 上启动即失败："无法定位程序输入点 CreateFile2 于 KERNEL32.dll"。
//
// 实测各用法的代价（clang-cl + /MD，dumpbin /imports 检查 CreateFile2）：
//   std::filesystem::path 构造/parent_path/wstring/c_str  -> 干净
//   is_regular_file / exists / is_directory / remove /
//   file_size / directory_iterator                        -> 引入 CreateFile2
//   current_path                                          -> 干净
//
// 所以这里用 GetFileAttributesW / DeleteFileW 实现同一语义：零分配、无句柄打开、
// 且从 Windows 2000 起就在 kernel32 里。判断语义与 std::filesystem 一致：
// 符号链接/目录重解析点不会被当成普通文件。
bool PathIsRegularFile(const std::filesystem::path& path);
bool PathIsDirectory(const std::filesystem::path& path);
bool PathExists(const std::filesystem::path& path);

// 删除一个文件；不存在也算成功（与 std::filesystem::remove 的用法一致：
// 调用方只关心"删完之后它不在"）。
bool RemoveFile(const std::filesystem::path& path);

// 当前工作目录（替代 std::filesystem::current_path）
std::filesystem::path CurrentDirectory();

// 形如 L"..."; 的 printf 封装，便于拼装日志行
std::wstring FormatW(const wchar_t* fmt, ...);

// 本地时间 HH:MM:SS，ASCII
std::wstring Timestamp();

// ---- CLI 输出的着色（cold & dark）------------------------------------------
//
// 与图形前端同一原则：一切正常时几乎不着色，只有真的需要人处理的东西才醒目：
// --melt / --dry-run / --selftest-* 的输出按行首标记分层：
//
//   [x] / FAIL...  -> 醒目的红色加粗（出错必定高亮）
//   [!]            -> 琥珀色（需要留意，但不致命）
//   其余（[*] [+] …）-> 白色
//
// 只在 stdout 是支持 VT 转义的控制台上启用：重定向到文件/管道时不着色（否则日志里全是
// 转义序列），Windows 7 的原生 conhost 上也不着色（它不解释这些序列，会打出一堆乱码）。
std::wstring CliPaint(const std::wstring& line);

}  // namespace secmelt
