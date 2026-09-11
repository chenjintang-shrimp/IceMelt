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
third_party/ntfs-3g/          改造版 ntfs-3g（独立 xmake 工程，msys/cygwin 目标）
  ├── xmake.lua               构建脚本（-p msys + cygwin gcc，含 config.h 自动生成）
  └── libntfs-3g/ src/ ntfsprogs/
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

### 构建 ntfs-3g

ntfs-3g 也走独立 xmake 工程，但目标平台是 **MSYS2 的 msys 子系统（Cygwin 兼容环境）**，
**不是** ucrt64 / mingw64 / clang64：

```sh
cd third_party/ntfs-3g
xmake f -P . --yes -p msys -a x86_64 --toolchain=gcc \
    --cc=D:/msys64/usr/bin/gcc.exe  --cxx=D:/msys64/usr/bin/g++.exe \
    --ld=D:/msys64/usr/bin/g++.exe --ar=D:/msys64/usr/bin/ar.exe
xmake build -P .
# -> build/msys/x86_64/release/{libntfs-3g.a, ntfsfix.exe, ntfscp.exe, ntfs-3g-cli.exe}
```

两个容易踩的点：

1. **必须显式指定 `--cc/--cxx/--ld/--ar`。** 否则 xmake 会用 PATH 里搜到的 gcc；若 PATH 中
   `ucrt64` 先于 `msys64/usr/bin`，就会静默选到 **ucrt64 的 mingw 版 gcc**，编出来的不是
   Cygwin 目标。`D:/msys64/usr/bin/gcc.exe` 的 target 是 `x86_64-pc-cygwin`。
2. **调用时 PATH 需包含 `D:\msys64\usr\bin`。** 否则 gcc 驱动 spawn 的 `cc1.exe` 找不到
   `msys-2.0.dll`，报 `error while loading shared libraries`，并连带使 `-fPIC` / `-MMD`
   等 flag 探测全部失败。在 MSYS 控制台里直接跑则无此问题。

产出物依赖 `msys-2.0.dll`（可在 dumpbin 的 dependents 里看到），这也反证目标是 Cygwin。

另外 `-std` 被钉到 **gnu17**：上游没指定 `-std`，而 gcc 15 默认 gnu23，C23 取消了隐式函数
声明等宽松规则，2022 年的老代码会大面积报错。

#### config.h 由 xmake 自动生成

`config.h` 是 autoconf 产物、构建必需（`-DHAVE_CONFIG_H`），但**不入库**：缺失时
`xmake.lua` 里的 `ntfs3g.config` 规则会自动调用 configure 把它生成出来，无需手工步骤。

生成用的命令**不能简化**，必须分两步：

```sh
./configure --disable-ntfs-3g --no-create --no-recursion
./config.status config.h
```

原因：直接跑 `./configure` 会在 `AC_OUTPUT` 阶段触发 `config.status --recheck`（把 configure
整个重跑一遍），而那种情况下产出的 `config.h` **与它自己的探测结果不一致** —— 实测日志里
`ac_cv_c_bigendian=no`，生成的 `config.h` 里 `WORDS_LITTLEENDIAN` 却是 `#undef`。这会打乱
`libntfs-3g/dir.c` 里 `index_union` 的类型定义，编译直接报 “incompatible type for argument 5
of `ntfs_filldir`”。用 `--no-create` 阻止 `AC_OUTPUT` 自动执行 config.status，再显式只生成
`config.h`，就绕开了这条路径。

在 MSYS 控制台里手工执行同样两步即可重建（想强制重建就先删掉 `config.h`）。

MSYS 的 msys 子系统未装 diffutils 时，configure 会打印 `cmp`/`diff: command not found`，
导致少数探测（如 `LSTAT_FOLLOWS_SLASHED_SYMLINK`、`HAVE_STDBOOL_H`）落空；这两个宏在本工程
编译的任何源码里都**未被使用**（全树 grep 无命中），可直接忽略。

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
