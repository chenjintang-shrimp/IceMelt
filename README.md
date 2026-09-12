<h1 align="center">SecMelt</h1>

<p align="center">用 KDU 与 WinDisk 破防各类冰点 / 还原软件</p>

**WinDisk** 是内核驱动，直接构造 SCSI 请求下发到磁盘 miniport，绕过文件系统，也绕过还原软件挂在上层的过滤驱动。
**KDU** 关掉驱动签名强制，让没有签名的 WinDisk 能加载。

两者配合，在 NTFS 层面摘掉还原软件的注册表痕迹、换掉 SYSTEM hive，然后立刻复位。不替换任何驱动文件。

## 它做什么

```
1 预检     提权 / DSE（开着就用 kdu -dse 0 自动关掉）/ WinDisk 驱动就位
2-3 载驱动  装载 WinDisk → 定位系统卷所在磁盘
4 名单      读 targets.txt（缺省用编译内置副本）
5 摘注册项  三个设备类的 Upper/LowerFilters（只删命中项）+ 删目标服务键
6 导出 hive RegSaveKeyEx(HKLM\SYSTEM) → 置干净 + 修校验和 → 结构校验
7 确认      列出将执行的动作（此时尚未发生任何裸盘写入）
8 写回      可选 ntfsfix → ntfscp 经 handle: 把 hive 写回 \Windows\System32\config\SYSTEM
9 清日志    清零 SYSTEM.LOG1/.LOG2 头部 4096 字节并读回断言全零
10 复位      bugcheck 0x0D000721（内核停住，不给注册表懒写回覆盖刚写入内容的机会）
```

## 构建

装好 xmake 和下面这些编译工具，一行 `xmake build` 一把梭：

```sh
pwsh -File configure.ps1                               # 检查依赖（可选，退出码可用于 CI）
xmake f --yes -p windows -a x64 --toolchain=clang-cl   # 只需一次
xmake build
```

| 依赖 | 用途 |
|---|---|
| Visual Studio + LLVM（clang-cl） | secmelt.exe |
| Windows Driver Kit | WinDisk.sys |
| mingw-w64 工具链（默认取 MSYS2 的 `D:\msys64\mingw64`，`MINGW_ROOT` 可指向任意发行版） | ntfs-3g 工具。需要 **msvcrt** 变体，不要 ucrt64 |
| 一个 POSIX shell（MSYS2 或 Git for Windows 的 bash 均可） | 仅首次生成 ntfs-3g 的 config.h 时用到 |
| `third_party/KDU` submodule | kdu.exe + drv64.dll；克隆后 `git submodule update --init --recursive` |

缺 WDK / mingw-w64 / KDU 时构建会打印 `skip ...` 并继续，子工程各自独立。
`configure.ps1` 把每一项实际探测一遍（包括 mingw 用的是 msvcrt 还是 UCRT 变体），
缺什么、怎么补都直接给出。

面向 Win7 时驱动要注意工程的目标平台：`Desktop` 可选 Windows7，新的 `Windows Driver` 平台强制 Win10+。
（微软 WDK 支持矩阵里只有 10.0.19041.5738 标注支持 Win7/8/8.1 驱动开发。）

产物是一套可以直接拷到目标机的组合：

```
build/windows/x64/release/
├── secmelt.exe
├── targets.txt            目标名单
├── WinDisk_x64.sys        驱动
├── kdu.exe                关 DSE
├── drv64.dll              kdu 的 provider 数据库，要和 kdu.exe 放一起
└── tools/                 ntfsfix / ntfscp / ntfs-3g-cli
```

单独构建某个子工程：`cd third_party/{WinDisk,ntfs-3g} && xmake f -P . --yes ... && xmake build -P .`；
KDU 的构建工程在 `third_party/KDU.build`（源码在 `third_party/KDU` submodule 里），用法相同。

## 用法

| 命令 | 作用 |
|---|---|
| `secmelt` | 交互式界面（Environment / Melt 两屏；`1`/`2` 切屏，`d` 预演，`m` 执行，`q` 退出） |
| `secmelt --dump` | 渲染一帧到 stdout 后退出，可用于 CI |
| `secmelt --dry-run` | 只读预演整条链路 |
| `secmelt --melt --yes-i-know` | 非交互执行整条链路；破坏性、不可回滚 |
| `secmelt --selftest-hive` | 校验 hive base block 偏移与校验和算法 |
| `secmelt --selftest-registry` | 验证过滤器摘除的写入路径 |
| `secmelt --selftest-raw` | 裸盘写入/读回判定（**只在虚拟机里跑**） |

`--selftest-raw`、`--melt` 和交互式 Melt 需要管理员权限；DSE 由程序自己关闭。

## 名单维护

`targets.txt`：构建期复制到 exe 旁边，运行时也可以直接改 exe 旁边那份。

```
# 语法：<名称>|<显示名>
#   # 开头 = 该条被禁用（保留在文件里但不会被处理）
DeepFrz|冰点还原（主驱动）
DfDiskLo|冰点还原（磁盘下过滤）
DFServ|冰点还原（服务）
SWFreeze|希沃冰点还原
#SWFrzAlbendazole|希沃冰点还原（备用）
PsVFilt|影子系统 PowerShadow
PsLFilt|影子系统 PowerShadow
```

只放注册项名（过滤器项名 / 服务键名），不放文件路径。

## FAQ

### 运行时

**kdu 输出 `Provider: "(null)"`，`-dse 0` 却像成功了？**
`drv64.dll` 没跟 `kdu.exe` 放在一起。缺了它 KDU 用一个空表，一个驱动都不会加载，退出码仍是 0。

**ConEmu 弹 `Max Real Console size was reached`？**
ConEmu 能放大的控制台大小 = 显示尺寸 ÷ 真控制台字体的单元格大小。把真控制台字体改小
（Settings → **Features** → "Debugging options" 里 "Show real console" 旁的 **...** → **Real console font**，
要用 TrueType 字体），或别最大化窗口，或把回滚缓冲设为 `h0`
（Settings → **Size and Pos** → 取消 "Long console output"）。

**TUI 闪 / 退出后清不干净？**
Win7 上 ConEmu 切不了备用屏幕，界面不依赖它 —— 退出后内容留在屏幕上属于正常。
把真控制台字体调小（见上一条）会更稳。

**TUI 启动后直接退出，退出码 3？**
当前控制台不解释 VT 转义序列，故意不启动以免刷屏。用 ConEmu / ANSICON，
或者走纯文本命令（`--dry-run` / `--melt --yes-i-know` / `--selftest-*` / `--dump`）。
强行启动：`SECMELT_TUI_FORCE=1`。

### 构建时

**`skip KDU ...` / `skip ntfs-3g tools ...`？**
KDU 需要 `git submodule update --init --recursive`；ntfs-3g 需要 mingw-w64 工具链 ——
MSYS2 里是 `pacman -S mingw-w64-x86_64-gcc`，或者用 `MINGW_ROOT` 指向别的发行版。

**只删了某个子工程的产物，`xmake build` 不补建？**
子工程挂在 secmelt 的链接步骤后面。用 `xmake build -r`，或进子目录单独构建。

**驱动在 Win7 上不加载？**
`dumpbin /headers WinDisk_x64.sys` 应为 `6.01 subsystem version`；显示 `10.00` 说明是
`SECMELT_WDK_WINVER=win10` 构建的。

## 许可

以 **GPL-3.0-or-later** 发布，见 `LICENSE`。第三方组件：`ntfs-3g`（GPL-2.0-or-later）、
`KDU`（MIT，submodule）、`WinDisk`（本项目）、`FTXUI`（MIT），均与 GPLv3 兼容。

## 免责声明

涉及内核驱动加载与裸盘写入，误用可导致数据损坏或系统无法启动（BSOD）。仅在你有权操作的机器与
磁盘上使用。第 8 步之后一旦开始写入就没有回滚路径：出错时程序会立刻触发 bugcheck 复位并报出
"系统状态未知"。
