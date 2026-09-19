// SecMelt —— 注册表摘除：设备类过滤器值与服务键
//
// 处理冻结产品会挂过滤器的 Windows 固定设备类（磁盘/卷/键盘/鼠标）的 UpperFilters / LowerFilters，
// 以及 HKLM\SYSTEM\CurrentControlSet\Services\<name> 服务键。
//
// 关键约束：过滤器值是字符串列表，只允许删掉命中名单的项，其余项必须原样保留 ——
// 整值删除会连系统自身的类驱动一起移除（本机实测 DiskDrive=[partmgr]、
// Keyboard=[kbdclass]、Mouse=[mouclass]）。
//
// 值的类型要原样保留：绝大多数安装程序写 REG_MULTI_SZ，但也有写单项 REG_SZ 的。
// 若只认 REG_MULTI_SZ，后者会被静默跳过 —— 那正是"报告成功但过滤器其实还在"的失败模式。

#pragma once

#include <string>
#include <vector>

namespace secmelt {

struct EditReport {
    std::vector<std::wstring> changedValues;  // "Class\\{GUID}\\UpperFilters"
    std::vector<std::wstring> deletedKeys;    // 实际存在并被删除的服务键（带 HKLM\ 前缀）
    std::vector<std::wstring> absentKeys;     // 不存在的服务键（正常情况，记录用）
    // 删键之前尝试停掉的服务：停成功的（含"本来就没在运行"）。
    std::vector<std::wstring> stoppedServices;
    // 停不下来的服务（仍在运行）。过滤器驱动大多没有 DriverUnload，这是常态而非故障 ——
    // 它的意义是"重启前保护仍然生效"，必须如实报出来，别让人以为已经失效了。
    std::vector<std::wstring> runningServices;
    // 写入/删除失败项（含 Win32 错误码）。调用方必须把它当作失败处理：
    // 注册项没摘干净却继续写回 hive，就只是白写一次磁盘。
    std::vector<std::wstring> failures;
};

// 单个过滤器值的现场状态
struct FilterProbe {
    std::wstring classGuid;
    std::wstring valueName;     // UpperFilters / LowerFilters
    bool present = false;       // 值是否存在
    unsigned long valueType = 0;  // REG_SZ / REG_MULTI_SZ（原样保留用）；DWORD 与 Reg API 对齐
    std::vector<std::wstring> items;     // 现有项（原顺序）
    std::vector<std::wstring> toRemove;  // 命中名单、将被移除的项
};

// 只读预演结果
struct ProbeReport {
    std::vector<FilterProbe> filters;  // kClasses 每个类 x {Upper,Lower}
    std::vector<std::wstring> existingServiceKeys;
    std::vector<std::wstring> absentServiceKeys;
};

// 只读探测：读 kClasses 里每个类键的过滤器值 + 探测服务键存在性。不调用任何写 API。
ProbeReport ProbeTargets(const std::vector<std::wstring>& names);

// 摘除逻辑的核心：对**一个**设备类键下的 UpperFilters / LowerFilters 做处理。
// classKeyPath 是相对 HKEY_LOCAL_MACHINE 的路径，classGuid 只用于报告字符串。
// StripFilterEntries 用 kClasses 里的真实设备类调用它；--selftest-registry 用一个 scratch 键调用它，
// 这样写入路径（字符串列表重写 / 整值删除）能在不碰设备类键的前提下被完整验证。
EditReport StripFilterValueIn(const std::wstring& classKeyPath, const std::wstring& classGuid,
                              const std::vector<std::wstring>& names);

// HKLM\SYSTEM\CurrentControlSet\Services\<name> 的完整路径（日志与确认清单用）
std::wstring ServiceKeyPath(const std::wstring& name);

// 过滤掉命中名单的项；整值删除仅发生在结果为空时。
EditReport StripFilterEntries(const std::vector<std::wstring>& names);

// 删除存在的服务键（整树），不存在的记入 absentKeys。
// 删键**之前**会先尝试停掉同名服务（见 StopService）—— 只删注册表键不会停下正在运行的驱动，
// 那样在重启前保护照旧生效，而我们可能马上就要绕过它。
EditReport DeleteServiceKeys(const std::vector<std::wstring>& names);

// 只**查询**一个服务是否在运行（只读，不做任何控制操作）。供 --dry-run 这类只读预演使用 ——
// 预演里绝不能真的去 ControlService。
// 返回 true 时 running 有效；false 表示查询失败，detail 里是原因。
bool QueryServiceRunning(const std::wstring& name, bool& running, std::wstring& detail);

// 停掉一个服务。只删除注册表键并不会停止已加载的驱动；先停掉它，保护才立刻失效。
// **停不下来不算失败**：内核过滤器驱动大多没有卸载例程（ControlService 会返回 1061），
// 那是常态 —— 重启后不加载才是真正的效果。只要 detail 里有原因就够，不当错误处理。
// 返回值：
//   true  + stopped=true  —— 已停止（含"本来就没在运行"、"没有这个服务"）
//   true  + stopped=false —— 服务仍在运行，detail 给原因
//   false                 —— 打开/控制服务失败，detail 里是 Win32 错误码
bool StopService(const std::wstring& name, bool& stopped, std::wstring& detail);

}  // namespace secmelt
