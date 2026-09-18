#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <vector>

namespace secmelt {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                           nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool EnablePrivilege(const wchar_t* name, std::wstring& error) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        error = FormatW(L"OpenProcessToken failed: %u", ::GetLastError());
        return false;
    }
    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, name, &luid)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(token);
        error = FormatW(L"LookupPrivilegeValue(%ls) failed: %u", name, err);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(token);
        error = FormatW(L"AdjustTokenPrivileges(%ls) failed: %u", name, err);
        return false;
    }
    // AdjustTokenPrivileges 在「特权不在令牌里」时返回 TRUE 但 LastError=ERROR_NOT_ALL_ASSIGNED
    const DWORD err = ::GetLastError();
    ::CloseHandle(token);
    if (err == ERROR_NOT_ALL_ASSIGNED) {
        error = FormatW(L"privilege %ls is not held by this token", name);
        return false;
    }
    return true;
}

std::filesystem::path ExeDir() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return CurrentDirectory();
        if (n < buf.size() - 1) return std::filesystem::path(buf.data(), buf.data() + n).parent_path();
        buf.resize(buf.size() * 2);
    }
}

// ---- 路径谓词（实现说明见 util.h）-----------------------------------------

namespace {

// 返回 INVALID_FILE_ATTRIBUTES 表示"不存在或不可访问"
DWORD AttributesOf(const std::filesystem::path& path) {
    return ::GetFileAttributesW(path.c_str());
}

}  // namespace

bool PathExists(const std::filesystem::path& path) {
    return AttributesOf(path) != INVALID_FILE_ATTRIBUTES;
}

bool PathIsDirectory(const std::filesystem::path& path) {
    const DWORD attrs = AttributesOf(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool PathIsRegularFile(const std::filesystem::path& path) {
    const DWORD attrs = AttributesOf(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    // 目录不是普通文件；设备类特殊文件（管道/控制台）也不该被当成"要读写的文件"。
    // 目录重解析点（符号链接/junction）由 GetFileAttributesW 返回目标属性，语义与
    // std::filesystem::is_regular_file 的 follow 行为一致。
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RemoveFile(const std::filesystem::path& path) {
    if (::DeleteFileW(path.c_str())) return true;
    // 已经不在了（或本来就不是文件）都算达成目的
    const DWORD err = ::GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

std::filesystem::path CurrentDirectory() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), buf.data());
        if (n == 0) return {};
        if (n < buf.size()) return std::filesystem::path(buf.data(), buf.data() + n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring FormatW(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    const int need = _vscwprintf(fmt, probe);
    va_end(probe);
    if (need <= 0) {
        va_end(args);
        return {};
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    _vsnwprintf_s(out.data(), out.size() + 1, _TRUNCATE, fmt, args);
    va_end(args);
    return out;
}

namespace {

// stdout 该不该着色。
//
// 两种情况都要认，因为它们是两套不同的机制：
//   1. conhost 自己能解释 VT 转义（Windows 10 起才有 ENABLE_VIRTUAL_TERMINAL_PROCESSING）；
//   2. **终端程序自己解析 ANSI** —— ConEmu / ANSICON / Windows Terminal 都属此类。
//      Windows 7 的 conhost 没有 VT 支持，但 ConEmu 会自己解析这些序列，所以**不能只看
//      conhost 的能力**（这是之前颜色不生效的原因：Win7 上 SetConsoleMode 那个标志必然失败）。
//
// 拿不到真控制台（被重定向到文件/管道）就一律不着色，免得日志里混进转义序列。
bool CliColorsEnabled() {
    static const bool enabled = [] {
        const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == INVALID_HANDLE_VALUE || out == nullptr) return false;
        DWORD mode = 0;
        if (!::GetConsoleMode(out, &mode)) return false;  // 重定向：不是控制台

        // 1) conhost 原生 VT（Win10+）
        if ((mode & 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0) return true;
        if (::SetConsoleMode(out, mode | 0x0004)) return true;

        // 2) 终端自己解析 ANSI
        const auto envIs = [](const wchar_t* name, const wchar_t* value) {
            wchar_t buf[32] = {};
            const DWORD got = ::GetEnvironmentVariableW(name, buf, 32);
            if (got == 0 || got >= 32) return false;
            return _wcsicmp(buf, value) == 0;
        };
        const auto envAny = [](const wchar_t* name) {
            return ::GetEnvironmentVariableW(name, nullptr, 0) > 0;
        };
        // ConEmu 只在真的开了 ANSI 时才设 ConEmuANSI=ON —— 单看 ConEmuPID 会在关闭 ANSI
        // 的 ConEmu 里打出乱码。
        if (envIs(L"ConEmuANSI", L"ON")) return true;
        if (envAny(L"ANSICON")) return true;      // ANSICON 注入器
        if (envAny(L"WT_SESSION")) return true;   // Windows Terminal：原生 VT
        if (envAny(L"TERM_PROGRAM")) return true; // VS Code 等
        return false;
    }();
    return enabled;
}

enum class CliTier { Normal, Warning, Error };

CliTier ClassifyCliLine(const std::wstring& line) {
    if (line.rfind(L"[x]", 0) == 0) return CliTier::Error;
    if (line.rfind(L"FAIL", 0) == 0) return CliTier::Error;  // FAIL / FAILED: ...
    if (line.rfind(L"[!]", 0) == 0) return CliTier::Warning;
    return CliTier::Normal;
}

}  // namespace

std::wstring CliPaint(const std::wstring& line) {
    if (!CliColorsEnabled()) return line;
    const wchar_t* code = L"\x1b[97m";  // 白色
    switch (ClassifyCliLine(line)) {
        case CliTier::Error: code = L"\x1b[1;91m"; break;   // 加粗亮红
        case CliTier::Warning: code = L"\x1b[93m"; break;   // 亮黄
        case CliTier::Normal: break;
    }
    return std::wstring(code) + line + L"\x1b[0m";
}

std::wstring Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    wchar_t buf[16] = {};
    ::wcsftime(buf, sizeof(buf) / sizeof(buf[0]), L"%H:%M:%S", &local);
    return buf;
}

}  // namespace secmelt
