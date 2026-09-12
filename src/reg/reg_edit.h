// SecMelt —— 注册表摘除：设备类过滤器值与服务键
//
// 只处理三个 Windows 固定设备类（磁盘/键盘/鼠标）的 UpperFilters / LowerFilters，
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
    std::vector<FilterProbe> filters;  // 三个类 x {Upper,Lower}
    std::vector<std::wstring> existingServiceKeys;
    std::vector<std::wstring> absentServiceKeys;
};

// 只读探测：读三个类键的过滤器值 + 探测服务键存在性。不调用任何写 API。
ProbeReport ProbeTargets(const std::vector<std::wstring>& names);

// 摘除逻辑的核心：对**一个**设备类键下的 UpperFilters / LowerFilters 做处理。
// classKeyPath 是相对 HKEY_LOCAL_MACHINE 的路径，classGuid 只用于报告字符串。
// StripFilterEntries 用三个真实设备类调用它；--selftest-registry 用一个 scratch 键调用它，
// 这样写入路径（字符串列表重写 / 整值删除）能在不碰设备类键的前提下被完整验证。
EditReport StripFilterValueIn(const std::wstring& classKeyPath, const std::wstring& classGuid,
                              const std::vector<std::wstring>& names);

// HKLM\SYSTEM\CurrentControlSet\Services\<name> 的完整路径（日志与确认清单用）
std::wstring ServiceKeyPath(const std::wstring& name);

// 过滤掉命中名单的项；整值删除仅发生在结果为空时。
EditReport StripFilterEntries(const std::vector<std::wstring>& names);

// 删除存在的服务键（整树），不存在的记入 absentKeys。
EditReport DeleteServiceKeys(const std::vector<std::wstring>& names);

}  // namespace secmelt
