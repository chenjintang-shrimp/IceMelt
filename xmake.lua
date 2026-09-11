-- SecMelt —— UnFreeze 后继：基于磁盘级绕过的“解冻”工具链
--
-- 工具链：clang-cl（MSVC ABI 下的 Clang，需本机 Visual Studio + LLVM）
--
-- xmake v3 约束（实测差异）：
--   * add_requires 必须在根作用域，放进 target() 会硬报错
--   * 没有 check_cxflags / import / os.run，无法在 xmake.lua 里探测 flag 支持
--   * 工具链判定走 get_config("toolchain")

set_project("SecMelt")
set_version("0.1.0")

-- 整体以 GPLv3 发布。third_party/ntfs-3g 为 GPL-2.0-or-later（允许升到 v3），
-- third_party/KDU 为 MIT（可并入 GPLv3 作品）。见 LICENSE 与 README 的许可说明。
set_license("GPL-3.0-or-later")

set_languages("c++20")

add_rules("mode.debug", "mode.release")

-- 命令行 --toolchain 优先于本文件默认值；两者要一起看，否则 /utf-8 会静默丢失
local DEFAULT_TOOLCHAIN = "clang-cl"
set_toolchains(DEFAULT_TOOLCHAIN)

local function using_clang_cl()
    local configured = get_config("toolchain")
    if configured and configured ~= "" then
        return configured == "clang-cl"
    end
    return DEFAULT_TOOLCHAIN == "clang-cl"
end

-- FTXUI v7.0.3（xmake-repo，MIT）
add_requires("ftxui v7.0.3")

target("secmelt")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("ftxui")

    -- clang-cl 需要 /utf-8 才能正确解析源码中的 UTF-8 字面量
    if using_clang_cl() then
        add_cxflags("/utf-8", { force = true })
    end

    if is_plat("windows") then
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("advapi32", "shell32", "user32")
    end
