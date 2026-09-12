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

std::wstring Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    wchar_t buf[16] = {};
    ::wcsftime(buf, sizeof(buf) / sizeof(buf[0]), L"%H:%M:%S", &local);
    return buf;
}

}  // namespace secmelt
