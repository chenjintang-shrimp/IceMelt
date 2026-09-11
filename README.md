# SecMelt

“解冻”工具链的后续项目。目标是在**绕过还原/影子类软件（冰点还原 Deep Freeze、影子系统
PowerShadow 等）的磁盘过滤保护**这件事上，把 `UnFreeze` 系列项目里散落的几个组件收敛成一个
可维护的工程：TUI 前端 + 内核驱动 + NTFS 用户态工具 + 无签名驱动加载。

当前阶段：**骨架 + 驱动可构建**。TUI 可运行并做真实自检；WinDisk 驱动已能用
clang-cl + xmake 独立构建出 `WinDisk.sys`。**尚未接入**的是：驱动加载、DSE 关闭、
裸盘读写与文件操作（这些仍是后续工作）。

加载策略已定：**关闭 DSE 后走正常内核加载器**（SCM/`NtLoadDriver`），
而非 KDU 的手动映射（`-map`）。原因是正常加载器会完整处理 PE 加载、重定位、
导入解析（含 `HAL.dll`）与 CRT 初始化段，驱动无需任何改动；手动映射则要求驱动
被专门设计为 "driverless"（DriverEntry 参数无效、无 SEH、只解析 ntoskrnl 导入）。

## 目录结构

```
xmake.lua                     主程序构建脚本（clang-cl + FTXUI）
src/main.cpp                  TUI 前端（FTXUI），首屏为前置条件自检
third_party/WinDisk/          内核驱动（独立 xmake 工程）
  ├── xmake.lua               驱动构建脚本（wdk.env.wdm + wdk.driver 规则）
  └── *.cpp/*.h               驱动源码 —— 直发 SCSI 到 miniport（绕过文件系统）
third_party/ntfs-3g/          改造版 ntfs-3g —— 含 handle: 设备协议与 ntfs-3g-cli
third_party/KDU/              git submodule（hfiref0x/KDU）—— 关闭 DSE
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

### 构建内核驱动

WinDisk 是独立 xmake 工程（驱动产物与 TUI 主程序分开构建）。需要 **Windows Driver Kit**；
xmake 内置的 wdk 规则会自动探测 WDK 并注入 `km` / `shared` / `km/crt` 头路径与
`km/<arch>` 库路径。

```sh
cd third_party/WinDisk
xmake f -P . --yes -p windows -a x64 --toolchain=clang-cl
xmake build -P .
# -> build/windows/x64/release/WinDisk.sys
```

`-P .` 是必需的：否则 xmake 会向上找到父目录的工程根。

驱动工程里有两处刻意对齐原 `.vcxproj` 的开关，缺一不可（否则链接失败）：

| 设置 | 原因 |
|---|---|
| `add_cxflags("/GS-")` | 开 `/GS` 会引用 `__security_check_cookie`，从而从 `BufferOverflowK.lib` 拉入 `gs_support.obj`；该 obj 需要一个**未修饰的 C 符号 `DriverEntry`**，与 C++ 修饰名冲突。原工程即 `BufferSecurityCheck=false`。 |
| `add_ldflags("-entry:DriverEntry")` | xmake 的 wdk 规则默认用 `GsDriverEntry`，同样会撞上未修饰名。原工程即 `EntryPointSymbol=DriverEntry`。 |

产出的 `WinDisk.sys` 与原 MSVC 产物对照：machine `0x8664`、subsystem NATIVE、导入
`ntoskrnl.exe`(45) + `HAL.dll`(`HalReturnToFirmware`) 完全一致。

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
