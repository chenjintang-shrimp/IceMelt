#include "raw/ntfs_tool.h"

#include <winioctl.h>

#include <cstring>
#include <vector>

#include "util.h"

namespace secmelt {
namespace {

// 把命令行拆成 CreateProcessW 需要的 argv（规则同 MSVCRT：空白分隔，双引号成组，
// 反斜杠仅在双引号前有转义含义）。工具路径与参数都由本程序构造，只需保证往返一致。
std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

}  // namespace

std::filesystem::path ResolveTool(const std::wstring& exeName) {
    std::error_code ec;
    const std::filesystem::path candidates[] = {
        ExeDir() / "tools" / exeName,
        ExeDir() / exeName,
    };
    for (const auto& candidate : candidates) {
        if (secmelt::PathIsRegularFile(candidate)) return candidate;
    }

    // PATH 兜底
    const DWORD need = ::SearchPathW(nullptr, exeName.c_str(), nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::vector<wchar_t> buffer(need);
        if (::SearchPathW(nullptr, exeName.c_str(), nullptr, need, buffer.data(), nullptr) > 0)
            return std::filesystem::path(buffer.data());
    }
    return {};
}

bool ListExtentsPhysical(const std::filesystem::path& file, const VolumeInfo& vol,
                         std::vector<Extent>& out, std::wstring& error) {
    out.clear();

    // 目标文件位于 config 等被 ACL 锁死的目录：必须用 backup 语义打开。
    std::wstring privilegeError;
    if (!EnablePrivilege(SE_BACKUP_NAME, privilegeError)) {
        error = FormatW(L"SE_BACKUP_NAME: %ls", privilegeError.c_str());
        return false;
    }

    HANDLE handle = ::CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES | GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = FormatW(L"CreateFileW(%ls) failed: %u", file.c_str(), ::GetLastError());
        return false;
    }

    const uint32_t clusterBytes = vol.sectorsPerCluster * vol.bytesPerSector;
    if (clusterBytes == 0) {
        ::CloseHandle(handle);
        error = L"volume geometry has zero cluster size";
        return false;
    }

    // FSCTL_GET_RETRIEVAL_POINTERS：每次拿一段，ERROR_MORE_DATA 表示还有后续段。
    // SDK 里的 RETRIEVAL_POINTERS_BUFFER.Extents 是匿名结构数组（没有 MAPPING_PAIR 这个名字）。
    constexpr size_t kExtentPairSize =
        sizeof(((RETRIEVAL_POINTERS_BUFFER*)nullptr)->Extents[0]);
    std::vector<unsigned char> buffer(offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) +
                                      kExtentPairSize * 64);
    STARTING_VCN_INPUT_BUFFER input{};
    input.StartingVcn.QuadPart = 0;

    for (;;) {
        DWORD returned = 0;
        const BOOL ok = ::DeviceIoControl(handle, FSCTL_GET_RETRIEVAL_POINTERS, &input,
                                          sizeof(input), buffer.data(),
                                          static_cast<DWORD>(buffer.size()), &returned, nullptr);
        const DWORD status = ::GetLastError();
        // 只有「成功」或「还有后续段」时 buffer 里才有有效内容；EOF 直接收尾。
        if (!ok && status != ERROR_MORE_DATA) {
            if (status == ERROR_HANDLE_EOF) {
                if (out.empty()) {
                    ::CloseHandle(handle);
                    error = FormatW(L"no physical extents reported for %ls", file.c_str());
                    return false;
                }
                break;
            }
            ::CloseHandle(handle);
            error = FormatW(L"FSCTL_GET_RETRIEVAL_POINTERS(%ls) failed: %u", file.c_str(), status);
            return false;
        }

        const auto* info = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
        const DWORD count = info->ExtentCount;
        if (count > 0) {
            const LONGLONG baseVcn = input.StartingVcn.QuadPart;
            for (DWORD i = 0; i < count; ++i) {
                const LONGLONG lcn = info->Extents[i].Lcn.QuadPart;
                const LONGLONG nextVcn = info->Extents[i].NextVcn.QuadPart;
                const LONGLONG startVcn = (i == 0) ? baseVcn : info->Extents[i - 1].NextVcn.QuadPart;
                if (lcn < 0) continue;  // 稀疏区段（Lcn == -1）：无物理位置，跳过
                Extent extent;
                extent.physicalOffset = static_cast<uint64_t>(lcn) * clusterBytes + vol.volumeOffset;
                extent.length = static_cast<uint64_t>(nextVcn - startVcn) * clusterBytes;
                out.push_back(extent);
            }
        }

        if (ok) break;
        const LONGLONG next = (count > 0) ? info->Extents[count - 1].NextVcn.QuadPart : 0;
        if (next <= input.StartingVcn.QuadPart) break;  // 无进展：防止死循环
        input.StartingVcn.QuadPart = next;
    }

    ::CloseHandle(handle);
    if (out.empty()) {
        error = FormatW(L"no physical extents reported for %ls", file.c_str());
        return false;
    }
    return true;
}

bool RunTool(const std::filesystem::path& exe, const std::wstring& args, int& exitCode,
             std::wstring& output) {
    exitCode = -1;
    output.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    // 只让写端可继承：读端留在本进程，避免子进程持有它导致读不到 EOF
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // STARTF_USESTDHANDLES 要求三个句柄都可继承；控制台的 stdin 未必是，故统一给 NUL。
    HANDLE nulIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);

    std::wstring command = QuoteArg(exe.wstring());
    if (!args.empty()) command += L" " + args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = (nulIn == INVALID_HANDLE_VALUE) ? ::GetStdHandle(STD_INPUT_HANDLE) : nulIn;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    const BOOL started = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                          TRUE /* 继承句柄：handle: 协议依赖它 */,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(writeEnd);
    if (nulIn != INVALID_HANDLE_VALUE) ::CloseHandle(nulIn);
    if (!started) {
        ::CloseHandle(readEnd);
        output = FormatW(L"CreateProcessW(%ls) failed: %u", command.c_str(), ::GetLastError());
        return false;
    }

    std::string raw;
    char chunk[1024];
    DWORD read = 0;
    while (::ReadFile(readEnd, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        raw.append(chunk, read);
    }
    ::CloseHandle(readEnd);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    // 工具输出多为 ASCII，但含中文路径时会随工具自身的编码而变（msys 版 ntfs-3g 输出
    // UTF-8，Windows 原生工具输出 ANSI）。严格按 UTF-8 解一次，失败再按 ANSI 解。
    std::wstring widened;
    {
        const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(),
                                              static_cast<int>(raw.size()), nullptr, 0);
        if (need > 0 || raw.empty()) {
            widened.assign(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(),
                                  static_cast<int>(raw.size()), widened.data(), need);
        } else {
            const int ansi = ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(),
                                                   static_cast<int>(raw.size()), nullptr, 0);
            if (ansi > 0) {
                widened.assign(static_cast<size_t>(ansi), L'\0');
                ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(), static_cast<int>(raw.size()),
                                      widened.data(), ansi);
            }
        }
    }
    output = widened;
    exitCode = static_cast<int>(code);
    return true;
}

std::wstring HandleSpec(HANDLE rawDevice, const VolumeInfo& vol) {
    return FormatW(L"handle:%zu:%llu:%llu", static_cast<size_t>(reinterpret_cast<uintptr_t>(rawDevice)),
                   vol.volumeOffset, vol.volumeLength);
}

bool NtfsFix(HANDLE rawDevice, const VolumeInfo& vol, int& exitCode, std::wstring& output,
             std::wstring& error) {
    const std::filesystem::path tool = ResolveTool(L"ntfsfix.exe");
    if (tool.empty()) {
        error = L"ntfsfix.exe not found (looked in <exeDir>/tools, <exeDir>, PATH)";
        return false;
    }
    const std::wstring args = L"\"" + HandleSpec(rawDevice, vol) + L"\"";
    if (!RunTool(tool, args, exitCode, output)) {
        error = output;
        return false;
    }
    return true;
}

// Windows 路径 -> NTFS 卷内路径（"C:\a\b" -> "/a/b"）。
// ntfscp 的 dest 只认 '/' 分隔符（libntfs-3g 的 PATH_SEP 就是 '/'），
// 传反斜杠会把整串当成一个文件名。
std::wstring ToNtfsPath(const std::wstring& windowsPath) {
    // ntfscp 的 dest 是卷内路径，且 ntfs_pathname_to_inode 只把 '/' 当分隔符
    // （PATH_SEP 定义为 '/'），反斜杠会被当成文件名字符。所以：
    //   C:\Windows\System32\config\SYSTEM  ->  /Windows/System32/config/SYSTEM
    std::wstring path = windowsPath;
    const size_t colon = path.find(L':');
    if (colon != std::wstring::npos && colon + 1 < path.size() &&
        (path[colon + 1] == L'\\' || path[colon + 1] == L'/')) {
        path = path.substr(colon + 1);
    }
    for (wchar_t& c : path) {
        if (c == L'\\') c = L'/';
    }
    if (path.empty() || path[0] != L'/') path.insert(path.begin(), L'/');
    return path;
}

bool NtfsCopyIn(HANDLE rawDevice, const VolumeInfo& vol, const std::filesystem::path& localSrc,
                const std::wstring& ntfsDest, int& exitCode, std::wstring& output,
                std::wstring& error) {
    const std::filesystem::path tool = ResolveTool(L"ntfscp.exe");
    if (tool.empty()) {
        error = L"ntfscp.exe not found (looked in <exeDir>/tools, <exeDir>, PATH)";
        return false;
    }
    const std::wstring args = L"-f -v \"" + HandleSpec(rawDevice, vol) + L"\" " +
                              QuoteArg(localSrc.wstring()) + L" " + QuoteArg(ToNtfsPath(ntfsDest));
    if (!RunTool(tool, args, exitCode, output)) {
        error = output;
        return false;
    }
    return true;
}

}  // namespace secmelt
