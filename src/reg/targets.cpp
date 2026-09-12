#include "reg/targets.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "util.h"

namespace secmelt {
namespace {

// 内置副本 == config/targets.txt（同一份内容的两处落点：文件便于运行时维护，
// 内置副本保证 exe 被单独拷走时仍可工作）。
const std::vector<Target>& BuiltinList() {
    static const std::vector<Target> kList = {
        {L"DeepFrz", L"冰点还原（主驱动）"},
        {L"DfDiskLo", L"冰点还原（磁盘下过滤）"},
        {L"DFServ", L"冰点还原（服务）"},
        {L"SWFreeze", L"希沃冰点还原"},
        {L"PsVFilt", L"影子系统 PowerShadow"},
        {L"PsLFilt", L"影子系统 PowerShadow"},
    };
    return kList;
}

std::wstring TrimW(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (::towlower(a[i]) != ::towlower(b[i])) return false;
    }
    return true;
}

}  // namespace

const std::vector<Target>& DefaultTargets() { return BuiltinList(); }

std::vector<Target> ParseTargets(const std::string& utf8Text, std::vector<std::wstring>* problems) {
    std::vector<Target> out;
    std::wistringstream lines(Widen(utf8Text));
    std::wstring line;
    size_t lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        const std::wstring trimmed = TrimW(line);
        // 空行与注释行（'#' 起始）都跳过：注释正是「保留但禁用」的表达方式
        if (trimmed.empty() || trimmed[0] == L'#') continue;

        const size_t bar = trimmed.find(L'|');
        const std::wstring name = TrimW(bar == std::wstring::npos ? trimmed : trimmed.substr(0, bar));
        std::wstring label = bar == std::wstring::npos ? std::wstring() : TrimW(trimmed.substr(bar + 1));

        if (name.empty()) {
            if (problems) problems->push_back(FormatW(L"line %zu: empty name, skipped", lineNo));
            continue;
        }
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const Target& t) {
            return EqualsNoCase(t.name, name);
        });
        if (duplicate) {
            if (problems) problems->push_back(FormatW(L"line %zu: duplicate name '%ls', ignored", lineNo, name.c_str()));
            continue;
        }
        if (label.empty()) label = name;
        out.push_back({name, label});
    }
    return out;
}

std::vector<Target> LoadTargets(const std::filesystem::path& exeDir,
                                std::vector<std::filesystem::path>* searched) {
    const std::filesystem::path candidates[] = {
        exeDir / "targets.txt",
        exeDir / "config" / "targets.txt",
    };

    std::error_code ec;
    for (const auto& path : candidates) {
        if (searched) searched->push_back(path);
        if (!secmelt::PathIsRegularFile(path)) continue;

        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        auto targets = ParseTargets(buffer.str());
        if (!targets.empty()) return targets;
    }
    return BuiltinList();
}

std::vector<std::wstring> TargetNames(const std::vector<Target>& targets) {
    std::vector<std::wstring> names;
    names.reserve(targets.size());
    for (const auto& t : targets) names.push_back(t.name);
    return names;
}

}  // namespace secmelt
