#include "reg/reg_edit.h"

#include <windows.h>

#include <cwctype>

#include "util.h"

namespace secmelt {
namespace {

// Windows 固定的设备类 GUID（磁盘/键盘/鼠标）。这三类正是冻结类软件挂过滤器的位置。
constexpr const wchar_t* kClasses[] = {
    L"{4D36E967-E325-11CE-BFC1-08002BE10318}",  // DiskDrive
    L"{4D36E96B-E325-11CE-BFC1-08002BE10318}",  // Keyboard
    L"{4D36E96F-E325-11CE-BFC1-08002BE10318}",  // Mouse
};

constexpr const wchar_t* kValueNames[] = {L"UpperFilters", L"LowerFilters"};

std::wstring ClassPath(const wchar_t* guid) {
    return FormatW(L"SYSTEM\\CurrentControlSet\\Control\\Class\\%ls", guid);
}

std::wstring ServicePath(const std::wstring& name) {
    return FormatW(L"SYSTEM\\CurrentControlSet\\Services\\%ls", name.c_str());
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && ::_wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool IsMatch(const std::vector<std::wstring>& names, const std::wstring& item) {
    for (const auto& n : names) {
        if (EqualsNoCase(n, item)) return true;
    }
    return false;
}

// 读一个过滤器值：REG_MULTI_SZ 取全部项，REG_SZ 取单项。
// 值不存在时 present=false（不是错误）；类型不受支持或读失败时返回 false 并写 problem。
// 只申请 KEY_QUERY_VALUE —— 这个函数也服务于只读预演，不该要求写权限。
bool ReadFilterValue(const std::wstring& subkey, const wchar_t* valueName,
                     std::vector<std::wstring>& items, bool& present, DWORD& type,
                     std::wstring& problem) {
    present = false;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return true;  // 键不存在：当作「值不存在」
    }

    type = 0;
    DWORD bytes = 0;
    LSTATUS status = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        ::RegCloseKey(key);
        return true;
    }
    if (status != ERROR_SUCCESS) {
        ::RegCloseKey(key);
        problem = FormatW(L"RegQueryValueExW(%ls) size probe failed: %ld", valueName, status);
        return false;
    }
    if (type != REG_MULTI_SZ && type != REG_SZ) {
        ::RegCloseKey(key);
        problem = FormatW(L"%ls is type %lu, not a string type", valueName, type);
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    DWORD read = bytes;
    status = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                reinterpret_cast<LPBYTE>(buffer.data()), &read);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        problem = FormatW(L"RegQueryValueExW(%ls) read failed: %ld", valueName, status);
        return false;
    }

    present = true;
    if (type == REG_SZ) {
        // 单项值（含结尾 NUL）；空串视为无项
        const std::wstring item(buffer.data(), read / sizeof(wchar_t));
        const size_t end = item.find(L'\0');
        const std::wstring trimmed = end == std::wstring::npos ? item : item.substr(0, end);
        if (!trimmed.empty()) items.push_back(trimmed);
        return true;
    }

    const wchar_t* p = buffer.data();
    const wchar_t* stop = buffer.data() + read / sizeof(wchar_t);
    while (p < stop && *p != L'\0') {
        const std::wstring item(p);
        if (!item.empty()) items.push_back(item);
        p += item.size() + 1;
    }
    return true;
}

// 写回过滤器值。原类型是 REG_SZ 且只剩一项时按 REG_SZ 写回，其余情况用 REG_MULTI_SZ
// （REG_SZ 装不下多项）。
bool WriteFilterValue(const std::wstring& subkey, const wchar_t* valueName, DWORD originalType,
                      const std::vector<std::wstring>& items, std::wstring& error) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        error = FormatW(L"RegOpenKeyExW(%ls) for write failed: %u", subkey.c_str(), ::GetLastError());
        return false;
    }

    std::vector<wchar_t> buffer;
    const bool singleSz = originalType == REG_SZ && items.size() == 1;
    if (singleSz) {
        buffer.assign(items[0].begin(), items[0].end());
        buffer.push_back(L'\0');
    } else {
        // REG_MULTI_SZ 的存储形态：各项以 NUL 分隔，末尾再加一个 NUL 结束符
        for (const auto& item : items) {
            buffer.insert(buffer.end(), item.begin(), item.end());
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');
    }

    const DWORD writeType = singleSz ? REG_SZ : REG_MULTI_SZ;
    const LSTATUS status = ::RegSetValueExW(
        key, valueName, 0, writeType, reinterpret_cast<const BYTE*>(buffer.data()),
        static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = FormatW(L"RegSetValueExW(%ls\\%ls) failed: %ld", subkey.c_str(), valueName, status);
        return false;
    }
    return true;
}

bool DeleteValue(const std::wstring& subkey, const wchar_t* valueName, std::wstring& error) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        error = FormatW(L"RegOpenKeyExW(%ls) for delete failed: %u", subkey.c_str(), ::GetLastError());
        return false;
    }
    const LSTATUS status = ::RegDeleteValueW(key, valueName);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        error = FormatW(L"RegDeleteValueW(%ls\\%ls) failed: %ld", subkey.c_str(), valueName, status);
        return false;
    }
    return true;
}

}  // namespace

std::wstring ServiceKeyPath(const std::wstring& name) {
    return FormatW(L"HKLM\\%ls", ServicePath(name).c_str());
}

ProbeReport ProbeTargets(const std::vector<std::wstring>& names) {
    ProbeReport report;
    for (const wchar_t* guid : kClasses) {
        const std::wstring path = ClassPath(guid);
        for (const wchar_t* valueName : kValueNames) {
            FilterProbe probe;
            probe.classGuid = guid;
            probe.valueName = valueName;
            std::wstring problem;
            if (!ReadFilterValue(path, valueName, probe.items, probe.present, probe.valueType,
                                 problem)) {
                continue;  // 类型异常/读失败：写路径同样跳过并记录失败
            }
            for (const auto& item : probe.items) {
                if (IsMatch(names, item)) probe.toRemove.push_back(item);
            }
            report.filters.push_back(std::move(probe));
        }
    }
    for (const auto& name : names) {
        HKEY key = nullptr;
        const std::wstring path = ServicePath(name);
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
            ::RegCloseKey(key);
            report.existingServiceKeys.push_back(name);
        } else {
            report.absentServiceKeys.push_back(name);
        }
    }
    return report;
}

EditReport StripFilterValueIn(const std::wstring& classKeyPath, const std::wstring& classGuid,
                              const std::vector<std::wstring>& names) {
    EditReport report;
    for (const wchar_t* valueName : kValueNames) {
        FilterProbe probe;
        probe.classGuid = classGuid;
        probe.valueName = valueName;
        std::wstring problem;
        if (!ReadFilterValue(classKeyPath, valueName, probe.items, probe.present, probe.valueType,
                             problem)) {
            // 读不了就绝不能假装「没有要摘的项」：当成失败让调用方中止
            report.failures.push_back(problem);
            continue;
        }
        if (!probe.present) continue;  // 值不存在：正常情况，跳过不报错

        std::vector<std::wstring> kept;
        kept.reserve(probe.items.size());
        for (const auto& item : probe.items) {
            if (!IsMatch(names, item)) kept.push_back(item);
        }
        if (kept.size() == probe.items.size()) continue;  // 名单里没有命中项，保持原样

        std::wstring error;
        if (kept.empty()) {
            // 整值删除只发生在过滤后为空时：本机实测三类都保留着系统自身的类驱动
            // （partmgr / kbdclass / mouclass），非空结果绝不会走到这里。
            if (!DeleteValue(classKeyPath, valueName, error)) {
                report.failures.push_back(error);
                continue;
            }
        } else if (!WriteFilterValue(classKeyPath, valueName, probe.valueType, kept, error)) {
            report.failures.push_back(error);
            continue;
        }
        report.changedValues.push_back(FormatW(L"Class\\%ls\\%ls", classGuid.c_str(), valueName));
    }
    return report;
}

EditReport StripFilterEntries(const std::vector<std::wstring>& names) {
    EditReport report;
    for (const wchar_t* guid : kClasses) {
        const EditReport one = StripFilterValueIn(ClassPath(guid), guid, names);
        report.changedValues.insert(report.changedValues.end(), one.changedValues.begin(),
                                    one.changedValues.end());
        report.failures.insert(report.failures.end(), one.failures.begin(), one.failures.end());
    }
    return report;
}

EditReport DeleteServiceKeys(const std::vector<std::wstring>& names) {
    EditReport report;
    for (const auto& name : names) {
        const std::wstring path = ServicePath(name);
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
            report.absentKeys.push_back(ServiceKeyPath(name));
            continue;
        }
        ::RegCloseKey(key);
        const LSTATUS status = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str());
        if (status == ERROR_SUCCESS) {
            report.deletedKeys.push_back(ServiceKeyPath(name));
        } else {
            report.failures.push_back(
                FormatW(L"RegDeleteTreeW(%ls) failed: %ld", ServiceKeyPath(name).c_str(), status));
        }
    }
    return report;
}

}  // namespace secmelt
