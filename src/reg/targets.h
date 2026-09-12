// SecMelt —— 目标名单（可维护）
//
// 名单由 exe 同目录的文本文件维护，缺省内置一份编译副本（源自原作者 UnFreeze 项目）。
// 文件语法：
//
//   # 注释行（该条被禁用，保留在文件里但不会被处理）
//   空行忽略；行首尾空白忽略；名称大小写不敏感
//   <名称>|<显示名>      显示名可省略，省略时用名称自身
//
// 注释行上「# 后紧跟内容」即视为禁用条目 —— 与 txt 的注释语义合二为一：名单文件里
// 被注释掉的驱动正是原作者注释掉的那几条，两者的表达方式因此保持一致。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace secmelt {

struct Target {
    std::wstring name;   // 服务键名 / 过滤器项名（大小写不敏感）
    std::wstring label;  // 人类可读的显示名
};

// 编译内置的名单（config/targets.txt 的副本）
const std::vector<Target>& DefaultTargets();

// 依次尝试 <exeDir>/targets.txt、<exeDir>/config/targets.txt。
// 两个都不存在时返回 DefaultTargets()。searched 非空时回填实际查过的路径（供日志记录）。
std::vector<Target> LoadTargets(const std::filesystem::path& exeDir,
                               std::vector<std::filesystem::path>* searched = nullptr);

// 解析一份名单文本；出错的行（非法/重复）通过 problems 回填。空文本返回空表。
std::vector<Target> ParseTargets(const std::string& utf8Text,
                                 std::vector<std::wstring>* problems = nullptr);

// 只需要名称列表时的便捷包装
std::vector<std::wstring> TargetNames(const std::vector<Target>& targets);

}  // namespace secmelt
