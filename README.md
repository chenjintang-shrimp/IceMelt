# SecMelt

“解冻”工具链的后续项目。目标是在**绕过还原/影子类软件（冰点还原 Deep Freeze、影子系统
PowerShadow 等）的磁盘过滤保护**这件事上，把 `UnFreeze` 系列项目里散落的几个组件收敛成一个
可维护的工程：TUI 前端 + 内核驱动 + NTFS 用户态工具 + 无签名驱动加载。

当前阶段：**工程骨架**。TUI 可运行并做真实的自检，内核/驱动加载/裸盘读写尚未接入。

## 目录结构

```
xmake.lua                 构建脚本（clang-cl + FTXUI）
src/main.cpp              TUI 前端（FTXUI），首屏为前置条件自检
third_party/WinDisk/      内核驱动源码 —— 直接对磁盘 miniport 发 SCSI 请求（绕过文件系统）
third_party/ntfs-3g/      改造版 ntfs-3g —— 含 handle: 设备协议与 ntfs-3g-cli
third_party/KDU/          git submodule（hfiref0x/KDU）—— 无 test-signing 加载未签名驱动
```

### 各组件在原方案中的角色

| 组件 | 作用 |
|---|---|
| **WinDisk** | WDM 驱动。`IoGetLowerDeviceObject` 走到磁盘 miniport，手工构造 `SCSI_REQUEST_BLOCK` 直发 `IRP_MJ_SCSI`；附带内核态强制关句柄（`CTL_UNLOCK_FILE`）与硬重启（`CTL_REBOOT_SYSTEM`）。绕过还原软件挂在上层的过滤驱动。 |
| **ntfs-3g（改造）** | `libntfs-3g/win32_io.c` 增加 `handle:<句柄>:<卷偏移>:<长度>` 设备协议 —— 用户态 ntfsfix / ntfscp / ntfs-3g-cli 可把 WinDisk 的设备句柄当磁盘用，直接操作 NTFS 磁盘结构。`src/ntfs-3g-cli.c` 是自研的管道协议 CLI。 |
| **KDU** | 用已签名的漏洞驱动（victim）取得内核原语，关闭 `CI!g_CiOptions`，使未签名驱动可被正常加载。 |

## 构建

前置：Visual Studio + LLVM（clang-cl）、xmake v3。

```sh
xmake f --yes -p windows -a x64 --toolchain=clang-cl
xmake build
```

FTXUI v7.0.3 由 xmake-repo 自动获取（MIT）。

### 运行

```sh
secmelt           # 交互式界面
secmelt --dump    # 渲染一帧到标准输出后退出（无 TTY 环境下的自检/回归用）
```

`--dump` 在全部前置条件满足时返回 0，否则返回 1 —— 可直接用于 CI 断言。

## 许可

本项目整体以 **GPL-3.0-or-later** 发布，见 `LICENSE`。

三方组件的许可与兼容性（已逐项核对源码头部，非推测）：

| 组件 | 许可 | 与 GPLv3 的兼容性 |
|---|---|---|
| `third_party/ntfs-3g` | **GPL-2.0-or-later** | 兼容。源码头部原文为 “either version 2 of the License, or (at your option) any later version”，明确给出升级到 v3 的选项。 |
| `third_party/KDU` | **MIT** | 兼容。MIT 是宽松许可，其代码可并入 GPLv3 作品，但 KDU 部分仍保留 MIT 声明。 |
| `third_party/WinDisk` | 本项目自带 | 由本项目以 GPL-3.0-or-later 发布。 |
| FTXUI（构建期获取） | MIT | 兼容。 |

注意：`third_party/ntfs-3g/` 内保留了上游的 `COPYING`(GPLv2) 与 `COPYING.LIB`，以及各源文件
原有的版权头，均未被改动 —— 上游归属与许可条款应保持原样。

本仓库中的 `third_party/KDU` 是 git submodule，指向 `hfiref0x/KDU` 的上游提交，其自身许可
（MIT）与版权归上游作者所有。

## 免责声明

涉及内核驱动加载与裸盘写入，误用可导致数据损坏或系统无法启动（BSOD）。仅在你有权操作的
机器与磁盘上使用。
