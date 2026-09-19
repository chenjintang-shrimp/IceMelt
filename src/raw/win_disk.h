// SecMelt —— WinDisk 驱动装载与裸盘读写
//
// 设备与 IOCTL 契约的唯一来源是 third_party/WinDisk/Public.h：
//   设备符号链接  \\.\WDLink
//   CTL_CHANGE_TARGET_DISK  切换目标磁盘（入参为宽字符串 "\Device\HarddiskN\DR0"）
//
// 写路径：驱动在 IRP_MJ_WRITE 里做「读整扇区 → 覆盖 → 写回」，因此支持非扇区对齐的
// 偏移与长度；偏移是**磁盘绝对字节偏移**（卷偏移要由调用方加进去）。

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace secmelt {

// 卷在物理磁盘上的位置与几何（引导扇区 + IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）
struct VolumeInfo {
    uint32_t diskNumber = 0;
    uint64_t volumeOffset = 0;    // 卷在磁盘上的起始字节偏移
    uint64_t volumeLength = 0;    // 卷长度（字节）
    uint32_t sectorsPerCluster = 0;
    uint32_t bytesPerSector = 0;
};

class WinDiskDevice {
public:
    WinDiskDevice() = default;
    ~WinDiskDevice();
    WinDiskDevice(const WinDiskDevice&) = delete;
    WinDiskDevice& operator=(const WinDiskDevice&) = delete;

    // 打开 \\.\WDLink。句柄带 bInheritHandle=TRUE —— ntfsfix/ntfscp 的
    // "handle:H:OFF:LEN" 协议要求子进程能继承该句柄（见 ntfs_tool.h）。
    bool Open(std::wstring& error);

    // 切换驱动当前目标磁盘；diskNumber 为 0 基（\Device\Harddisk0）
    bool SetTargetDisk(uint32_t diskNumber, std::wstring& error);

    bool WriteAt(uint64_t byteOffset, const void* data, size_t len, std::wstring& error);
    bool ReadAt(uint64_t byteOffset, void* data, size_t len, std::wstring& error);

    // 关闭用户态设备句柄；调用方随后可删除 WinDisk 的 SCM 服务项。
    void Close();

    HANDLE raw() const { return handle_; }
    bool valid() const;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// 设备符号链接（\??\WDLink，来自 third_party/WinDisk/Public.h）。调用方需要它做日志
// 与 CreateFile，但不必因此包含三方头文件。
const wchar_t* DeviceSymbolicLink();

// 装载内核驱动服务的结果。判据就是 StartServiceW 的返回码本身：未签名的 WinDisk
// 只有驱动签名强制（DSE）真被解开才装得上，除非去装载，否则"CI 开着没有"都只是侧面推测。
enum class DriverLoad {
    Loaded,             // 已启动，或本来就是运行中
    SignatureRejected,  // StartServiceW 返回 577(ERROR_INVALID_IMAGE_HASH)：签名强制拦下了
    Failed,             // 其它错误（服务注册/启动失败、文件不存在…）
};

// 创建/更新内核驱动服务并启动。
// 服务已存在（ERROR_SERVICE_EXISTS）或已运行（ERROR_SERVICE_ALREADY_RUNNING）视为成功；
// ImagePath 与 sysPath 不一致时用 ChangeServiceConfigW 纠正。
DriverLoad LoadDriver(const std::filesystem::path& sysPath, const std::wstring& serviceName,
                      std::wstring& error);

// 停止并删除服务（尽力而为，用于清理）
bool UnloadDriver(const std::wstring& serviceName);

// 读卷的引导扇区与磁盘范围（如 L"\\\\.\\C:"）
bool QueryVolumeInfo(const std::wstring& volumePath, VolumeInfo& out, std::wstring& error);

}  // namespace secmelt
