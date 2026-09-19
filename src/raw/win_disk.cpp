#include "raw/win_disk.h"

#include <winioctl.h>

#include <cstring>
#include <vector>

#include "WinDisk/Public.h"  // DEVICE_NAME / SYMBOLIC_LINK_NAME / CTL_* 契约
#include "util.h"

namespace secmelt {
namespace {

// 驱动在 CTL_CHANGE_TARGET_DISK 里做的是：
//   Buffer = ExAllocatePool(InBufferLength + 1); RtlCopyMemory(Buffer, input, InBufferLength);
//   Buffer[InBufferLength / 2] = '\0';
// 即在 wchar 索引 InBufferLength/2 处写终止符 —— 这要求 InBufferLength/2*2 + 2 <= InBufferLength + 1，
// 对偶数长度恒差 1 字节。传奇数长度即可让它严格落在分配范围内，同时我们这边也多准备 1 字节。
std::vector<unsigned char> MakeIoctlString(const std::wstring& text) {
    const size_t nameBytes = text.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(nameBytes + 1, 0);
    std::memcpy(buffer.data(), text.data(), nameBytes);
    return buffer;
}

bool Ioctl(HANDLE device, DWORD code, const std::vector<unsigned char>& input,
           std::wstring& error, const wchar_t* what) {
    DWORD returned = 0;
    if (!::DeviceIoControl(device, code, const_cast<unsigned char*>(input.data()),
                           static_cast<DWORD>(input.size()), nullptr, 0, &returned, nullptr)) {
        error = FormatW(L"%ls failed: %u", what, ::GetLastError());
        return false;
    }
    return true;
}

}  // namespace

WinDiskDevice::~WinDiskDevice() { Close(); }

void WinDiskDevice::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

const wchar_t* DeviceSymbolicLink() { return SYMBOLIC_LINK_NAME; }

bool WinDiskDevice::valid() const { return handle_ != INVALID_HANDLE_VALUE; }

bool WinDiskDevice::Open(std::wstring& error) {
    if (handle_ != INVALID_HANDLE_VALUE) return true;

    // 句柄必须可继承：ntfsfix/ntfscp 的 "handle:H:OFF:LEN" 协议要求子进程拿到同一个卷句柄
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    handle_ = ::CreateFileW(SYMBOLIC_LINK_NAME, GENERIC_READ | GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0,
                            nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error = FormatW(L"CreateFileW(%ls) failed: %u", SYMBOLIC_LINK_NAME, ::GetLastError());
        return false;
    }
    return true;
}

bool WinDiskDevice::SetTargetDisk(uint32_t diskNumber, std::wstring& error) {
    if (!valid()) {
        error = L"device is not open";
        return false;
    }
    const std::wstring name = FormatW(L"\\Device\\Harddisk%u\\DR0", diskNumber);
    return Ioctl(handle_, CTL_CHANGE_TARGET_DISK, MakeIoctlString(name), error,
                 L"CTL_CHANGE_TARGET_DISK");
}

bool WinDiskDevice::WriteAt(uint64_t byteOffset, const void* data, size_t len, std::wstring& error) {
    if (!valid()) {
        error = L"device is not open";
        return false;
    }
    // 驱动支持非扇区对齐的偏移与长度（内部读整扇区→覆盖→写回），故不需要在这里对齐。
    for (int attempt = 0; attempt < 8; ++attempt) {
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(byteOffset);
        if (!::SetFilePointerEx(handle_, offset, nullptr, FILE_BEGIN)) {
            error = FormatW(L"SetFilePointerEx(%llu) failed: %u", byteOffset, ::GetLastError());
            return false;
        }
        DWORD written = 0;
        if (::WriteFile(handle_, data, static_cast<DWORD>(len), &written, nullptr) &&
            written == len) {
            return true;
        }
    }
    error = FormatW(L"WriteFile(%llu, %zu) failed after retries: %u", byteOffset, len,
                    ::GetLastError());
    return false;
}

bool WinDiskDevice::ReadAt(uint64_t byteOffset, void* data, size_t len, std::wstring& error) {
    if (!valid()) {
        error = L"device is not open";
        return false;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(byteOffset);
    if (!::SetFilePointerEx(handle_, offset, nullptr, FILE_BEGIN)) {
        error = FormatW(L"SetFilePointerEx(%llu) failed: %u", byteOffset, ::GetLastError());
        return false;
    }
    DWORD read = 0;
    if (!::ReadFile(handle_, data, static_cast<DWORD>(len), &read, nullptr) || read != len) {
        error = FormatW(L"ReadFile(%llu, %zu) failed: %u", byteOffset, len, ::GetLastError());
        return false;
    }
    return true;
}

DriverLoad LoadDriver(const std::filesystem::path& sysPath, const std::wstring& serviceName,
                      std::wstring& error) {
    if (!secmelt::PathIsRegularFile(sysPath)) {
        error = FormatW(L"driver file not found: %ls", sysPath.c_str());
        return DriverLoad::Failed;
    }

    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!manager) {
        error = FormatW(L"OpenSCManagerW failed: %u", ::GetLastError());
        return DriverLoad::Failed;
    }

    SC_HANDLE service = ::CreateServiceW(
        manager, serviceName.c_str(), serviceName.c_str(), SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, sysPath.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);


    if (!service) {
        const DWORD createError = ::GetLastError();
        if (createError != ERROR_SERVICE_EXISTS) {
            error = FormatW(L"CreateServiceW(%ls) failed: %u", serviceName.c_str(), createError);
            ::CloseServiceHandle(manager);
            return DriverLoad::Failed;
        }

        service = ::OpenServiceW(manager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!service) {
            error = FormatW(L"OpenServiceW(%ls) failed: %u", serviceName.c_str(), ::GetLastError());
            ::CloseServiceHandle(manager);
            return DriverLoad::Failed;
        }
        // 已存在的服务可能指向别处的旧驱动文件（本机实测就有指向 D:\新建文件夹 (3)\... 的残留），
        // 必须纠正，否则启动的是另一个二进制。
        if (!::ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                    sysPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr)) {
            error = FormatW(L"ChangeServiceConfigW(%ls) failed: %u", serviceName.c_str(),
                            ::GetLastError());
            ::CloseServiceHandle(service);
            ::CloseServiceHandle(manager);
            return DriverLoad::Failed;
        }
    }

    bool loaded = true;
    bool signatureRejected = false;
    if (!::StartServiceW(service, 0, nullptr)) {
        const DWORD startError = ::GetLastError();
        if (startError != ERROR_SERVICE_ALREADY_RUNNING) {
            loaded = false;
            // 577 就是"签名强制还在拦"。调用方拿它当 DSE 判据：先 kdu -dse 0，再重新
            // 装载一次 —— 能装上了才算真的关掉（查询 CI 状态只是侧面推测）。
            signatureRejected = startError == ERROR_INVALID_IMAGE_HASH;
            error = FormatW(L"StartServiceW(%ls) failed: %u%s", serviceName.c_str(), startError,
                            signatureRejected
                                ? L" (ERROR_INVALID_IMAGE_HASH: signature enforcement rejected "
                                  L"this unsigned driver)"
                                : L"");
        }
    }


    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    if (loaded) return DriverLoad::Loaded;
    return signatureRejected ? DriverLoad::SignatureRejected : DriverLoad::Failed;
}

bool UnloadDriver(const std::wstring& serviceName) {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!manager) return false;
    SC_HANDLE service = ::OpenServiceW(manager, serviceName.c_str(), SERVICE_ALL_ACCESS);
    if (!service) {
        ::CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    const bool ok = ::DeleteService(service) != FALSE;
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return ok;
}

bool QueryVolumeInfo(const std::wstring& volumePath, VolumeInfo& out, std::wstring& error) {
    HANDLE volume = ::CreateFileW(volumePath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                  nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        error = FormatW(L"CreateFileW(%ls) failed: %u", volumePath.c_str(), ::GetLastError());
        return false;
    }

    unsigned char boot[512] = {};
    DWORD read = 0;
    if (!::ReadFile(volume, boot, sizeof(boot), &read, nullptr) || read != sizeof(boot)) {
        error = FormatW(L"reading boot sector of %ls failed: %u", volumePath.c_str(), ::GetLastError());
        ::CloseHandle(volume);
        return false;
    }

    uint16_t bytesPerSector = 0;
    std::memcpy(&bytesPerSector, boot + 0x0B, sizeof(bytesPerSector));
    const uint32_t sectorsPerCluster = boot[0x0D];
    uint64_t numberSectors = 0;
    std::memcpy(&numberSectors, boot + 0x28, sizeof(numberSectors));

    if (bytesPerSector == 0 || sectorsPerCluster == 0) {
        error = FormatW(L"boot sector of %ls has invalid geometry (bps=%u, spc=%u)", volumePath.c_str(),
                        bytesPerSector, sectorsPerCluster);
        ::CloseHandle(volume);
        return false;
    }

    // 卷可能横跨多个 extent；这里只用第一个（系统卷的实际情形）并在日志中如实暴露数量
    std::vector<unsigned char> extents(offsetof(VOLUME_DISK_EXTENTS, Extents) +
                                       sizeof(DISK_EXTENT) * 8);
    DWORD returned = 0;
    if (!::DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, extents.data(),
                           static_cast<DWORD>(extents.size()), &returned, nullptr)) {
        error = FormatW(L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(%ls) failed: %u", volumePath.c_str(),
                        ::GetLastError());
        ::CloseHandle(volume);
        return false;
    }
    ::CloseHandle(volume);

    const auto* info = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extents.data());
    if (info->NumberOfDiskExtents < 1) {
        error = FormatW(L"%ls reports no disk extents", volumePath.c_str());
        return false;
    }

    out.diskNumber = info->Extents[0].DiskNumber;
    out.volumeOffset = info->Extents[0].StartingOffset.QuadPart;
    out.bytesPerSector = bytesPerSector;
    out.sectorsPerCluster = sectorsPerCluster;
    out.volumeLength = numberSectors * bytesPerSector;
    return true;
}

}  // namespace secmelt
