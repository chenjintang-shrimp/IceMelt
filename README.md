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

本项目使用 xmake 作为构建系统。xmake 是正确的！强大的 lua 脚本让我能方便的实现繁重的各种乱七八糟的东西，以及烦人的驱动编译处理条件。

| 依赖 | 用途 |
|---|---|
| Visual Studio + LLVM（clang-cl） | secmelt.exe |
| Windows Driver Kit | WinDisk.sys |
| mingw-w64 工具链（默认取 MSYS2 的 `D:\msys64\mingw64`，`MINGW_ROOT` 可指向任意发行版） | ntfs-3g 工具。需要 **msvcrt** 变体，不要 ucrt64 |
| PowerShell（7 或系统自带的 5.1 都行） | 首次构建时探测工具链、生成 ntfs-3g 的 config.h |
| `third_party/KDU` submodule | kdu.exe + drv64.dll；克隆后 `git submodule update --init --recursive` |

使用以下命令一把梭：

```sh
pwsh -File configure.ps1                               # 检查依赖（可选，退出码可用于 CI）
xmake f --yes -p windows -a x64 --toolchain=clang-cl   # 只需一次
xmake build
```

看输出，缺啥补啥，我自认为写的很清楚。cold & dark （没有任何引人注目的颜色）代表一切正常。

---

glibc 动态链接器有一段著名注释：

```
/* Now life is sane; we can call functions and access global data.
   Set up to use the operating system facilities, and find out from
   the operating system's program loader where to find the program
   header table in core.  Put the rest of _dl_start into a separate
   function, that way the compiler cannot put accesses to the GOT
   before ELF_DYNAMIC_RELOCATE.  */
...
/* Now life is peachy; we can do all normal operations.
   On to the real work.  */
```

你们能享受一把梭，是不知道我当初跟编译几个前置做了多少斗争。😠😡🤬

---

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

Q：**kdu 输出 `Provider: "(null)"`，`-dse 0` 却像成功了？**

A：`drv64.dll` 没跟 `kdu.exe` 放在一起。缺了它 KDU 用一个空表，一个驱动都不会加载，退出码仍是 0。

Q：**ConEmu 弹 `Max Real Console size was reached`？**

A：ConEmu 能放大的控制台大小 = 显示尺寸 ÷ 真控制台字体的单元格大小。把真控制台字体改小
（Settings → **Features** → "Debugging options" 里 "Show real console" 旁的 **...** → **Real console font**，
要用 TrueType 字体），或别最大化窗口，或把回滚缓冲设为 `h0`
（Settings → **Size and Pos** → 取消 "Long console output"）。

Q：**TUI 闪 / 退出后清不干净？**

A：Win7 上 ConEmu 切不了备用屏幕，界面不依赖它 —— 退出后内容留在屏幕上属于正常。
把真控制台字体调小（见上一条）会更稳。

Q：**TUI 启动后直接退出，退出码 3？**

A： 当前控制台不解释 VT 转义序列，故意不启动以免刷屏。用 ConEmu / ANSICON，
或者走纯文本命令（`--dry-run` / `--melt --yes-i-know` / `--selftest-*` / `--dump`）。
强行启动：`SECMELT_TUI_FORCE=1`。

### 构建时

Q：**`skip KDU ...` / `skip ntfs-3g tools ...`？**

A：KDU 需要 `git submodule update --init --recursive`；ntfs-3g 需要 mingw-w64 工具链 ——
MSYS2 里是 `pacman -S mingw-w64-x86_64-gcc`，或者用 `MINGW_ROOT` 指向别的发行版。

Q：**只删了某个子工程的产物，`xmake build` 不补建？**

A： 子工程挂在 secmelt 的链接步骤后面。用 `xmake build -r`，或进子目录单独构建。

Q：**驱动在 Win7 上不加载？**

A：`dumpbin /headers WinDisk_x64.sys` 应为 `6.01 subsystem version`；显示 `10.00` 说明是
`SECMELT_WDK_WINVER=win10` 构建的。

## 许可

以 **GPL-3.0-or-later** 发布，见 `LICENSE`。第三方组件：`ntfs-3g`（GPL-2.0-or-later）、
`KDU`（MIT，submodule）、`WinDisk`（本项目）、`FTXUI`（MIT），均与 GPLv3 兼容。

## 免责声明

涉及内核驱动加载与裸盘写入，误用可导致数据损坏或系统无法启动（BSOD）。仅在你有权操作的机器与
磁盘上使用。第 8 步之后一旦开始写入就没有回滚路径：出错时程序会立刻触发 bugcheck 复位并报出
"系统状态未知"。

## 后记

丙午年农历八月初二，项目业已落地。循其本，乃发于 2025，亦可朔至 2024 年八月。期间，承蒙各路大神指点，历经波折，或行时停，亦有巧思，亦遇困难，所幸始得 AI 之力，项目方成。

忆当初，以此发家，理论研究，亦有成果，可惜未成气候，也未出成品，以愚不擅 ui 编程，遂搁置。

几周之前，偶知 ftxui，眼前一亮，跃跃欲试，亦成气候，颇为顺手，遂得动力，重拾旧业，继续开发。高一学业繁忙，只得抽出暇余，断断续续，勉力为之，今日成也。

燕雀安知鸿鹄之志哉？余之志，不在高中一“多媒体管理员”，每日浑浑噩噩，为讲台所绊，为琐事所扰，碌碌无为，虚度光阴。余之志，在星辰大海！底层开发，操作系统，人工智能，网络安全。遂入软件开发之业，苦练竞赛，研工程之技巧，领算法之精髓，攀技术之高峰，潜底层之深海。三年磨一剑，以此，纪念我初中未竟之梦，破开束缚电教自由操作的厚厚坚冰。

也愿，以我之毅力，我之勇气，我之决心，破开附着于我心中的，那层坚冰。破茧成蝶，融冰成雪，IceMelt，由此得名。

chenjintang-shrimp/沧粟虾，
2026年9月12日
