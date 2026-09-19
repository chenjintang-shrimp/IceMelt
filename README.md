<h1 align="center">IceMelt</h1>

<p align="center">用 KDU 与 WinDisk 破防各类冰点 / 还原软件</p>

**WinDisk** 是内核驱动，直接构造 SCSI 请求下发到磁盘 miniport，绕过文件系统，也绕过还原软件挂在上层的过滤驱动。
**KDU** 关掉驱动签名强制，让没有签名的 WinDisk 能加载 —— 只在装载真的被 577 拒绝时才用得上
（判据见下：装载返回码，不是任何查询接口的读数）。

两者配合，在 NTFS 层面摘掉还原软件的注册表痕迹、换掉 SYSTEM hive，然后交回给你重启。不替换任何驱动文件。

## 它做什么

```
1 预检     提权 / WinDisk 驱动就位
2-3 载驱动  装载 WinDisk（被 577 拒绝 = 签名强制还在拦 → kdu -dse 0 → 重新装载）
            → 定位系统卷所在磁盘
4 名单      读 targets.txt（缺省用编译内置副本）
5 摘注册项  三个设备类的 Upper/LowerFilters（只删命中项）+ **先停服务再删**目标服务键
            （只删注册表键停不下已加载的驱动；停不下来不算失败，见下）
6 导出 hive RegSaveKeyEx(HKLM\SYSTEM) → 置干净 + 修校验和 → 结构校验
            → 从**导出的副本**里离线摘掉装载用的临时服务键（不改活动注册表：那一侧归
              SCM 管，动了会让注册表与 SCM 脱节 —— 实测会以 ChangeServiceConfigW failed: 2 炸掉）
7 确认      列出将执行的动作（此时尚未发生任何裸盘写入）
8 写回      **写回前守卫**（用 ntfs-3g 的**只读直接挂载**读目标，确认它是 regf hive，否则一个字节
            都不写就中止）→ ntfsfix（非 0 退出**不中止**，见下）
            → **回滚点**（先把当前 SYSTEM 整份读下来）→ **预演**（把同一份字节写到同目录的
              临时名字并读回校验，**不碰 SYSTEM**）→ ntfscp 经 handle: 把 hive 写回
              \Windows\System32\config\SYSTEM → **经 ntfs-3g 读回校验**（必须与导出的
              逐字节一致，且 base block 干净、校验和有效）
            → 任何一步失败：**回滚 + 不复位 + 退出**（见下）
8b 备份同步  同一份 hive 也写进 config\RegBack\SYSTEM（若存在），让主/备代数一致
9 日志       只读检查 SYSTEM.LOG / .LOG1 / .LOG2，**不改动**（见下：清零不改变任何结果）
10 收尾      置系统卷的 dirty 标记（autochk 下次启动即检查它，同 chkdsk /f）
            → 卸载驱动，把重启交回给你：**不自动复位**（普通重启同样生效，见下）
```

第 5 步之后若因故中止（hive 导出/校验失败、确认被拒），日志会明确点出：**活动注册表已经
被改过了**。那些改动在内存里，Windows 会在几秒内懒写回磁盘 —— 下次正常重启时过滤器驱动
已经不会加载，只是 SYSTEM hive 没有被重写。这是半成品状态，不是"什么都没发生"。

## 构建

本项目使用 xmake 作为构建系统。xmake 是正确的！强大的 lua 脚本让我能方便的实现繁重的各种乱七八糟的东西，以及烦人的驱动编译处理条件。

装好 xmake 和下面这些编译工具，一行 `xmake build` 一把梭：

```sh
pwsh -File configure.ps1                               # 检查依赖（可选，退出码可用于 CI）
xmake f --yes -p windows -a x64 --toolchain=clang-cl   # 只需一次
xmake build
```

看输出，缺啥补啥，我自认为写的很清楚。cold & dark （没有任何引人注目的颜色）代表一切正常。

| 依赖 | 用途 |
|---|---|
| Visual Studio + LLVM（clang-cl） | secmelt.exe |
| Windows Driver Kit | WinDisk.sys |
| mingw-w64 工具链（默认取 MSYS2 的 `D:\msys64\mingw64`，`MINGW_ROOT` 可指向任意发行版） | ntfs-3g 工具。需要 **msvcrt** 变体，不要 ucrt64 |
| PowerShell（7 或系统自带的 5.1 都行） | 首次构建时探测工具链、生成 ntfs-3g 的 config.h |
| `third_party/KDU` submodule | kdu.exe + drv64.dll；克隆后 `git submodule update --init --recursive` |

缺 WDK / mingw-w64 / KDU 时构建会打印 `skip ...` 并继续，子工程各自独立。
`configure.ps1` 把每一项实际探测一遍（包括 mingw 用的是 msvcrt 还是 UCRT 变体），
缺什么、怎么补都直接给出。

面向 Win7 时驱动要注意工程的目标平台：`Desktop` 可选 Windows7，新的 `Windows Driver` 平台强制 Win10+。
（微软 WDK 支持矩阵里只有 10.0.19041.5738 标注支持 Win7/8/8.1 驱动开发。）

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
├── secmelt-gui.exe        图形前端（可选，资产与 secmelt.exe 共用）
├── targets.txt            目标名单
├── WinDisk_x64.sys        驱动
├── kdu.exe                关 DSE
├── drv64.dll              kdu 的 provider 数据库，要和 kdu.exe 放一起
└── tools/                 ntfsfix / ntfscp / ntfs-3g-cli / mkntfs
```

单独构建某个子工程：`cd third_party/{WinDisk,ntfs-3g} && xmake f -P . --yes ... && xmake build -P .`；
KDU 的构建工程在 `third_party/KDU.build`（源码在 `third_party/KDU` submodule 里），用法相同。

两个前端产物（`secmelt` / `secmelt-gui`）挂的是同一段 `after_build`：**构建任意一个都会连带构建
三方子工程并把资产落位到该构建目录**（同一次 xmake 里两个一起构建时三方只跑一遍）。所以
`xmake build secmelt-gui` 单独跑也能得到完整的运行期组合。

### 打发布包

`package.ps1` 把上面这套组合拆成两个可直接分发的包（版本默认取 `xmake.lua` 的 `set_version`）：

```
dist/
├── IceMelt-v1.0.0-cli/      + .zip    命令行前端：secmelt.exe + preflight.bat / melt.bat + 运行期资产
└── IceMelt-GUI-v1.0.0-gui/  + .zip    图形前端：secmelt-gui.exe + 同一套运行期资产
```

```powershell
pwsh -File package.ps1                  # 版本取 xmake.lua
pwsh -File package.ps1 -Version v1.0.0  # CI 里传 tag 名
```

两个包各带一份完整的运行期资产（驱动 / kdu / 名单 / ntfs-3g 工具），目标机上只放其中一个就能跑；
缺任何一项脚本直接失败，不出半可用的包。CLI 包里那两个 `.bat` 调的是自己旁边的 exe（`%~dp0`），
双击即用、结尾停住等按键 —— 但它们不自己弹 UAC，需要提权的动作请右键"以管理员身份运行"。
CI（`.github/workflows/build.yml`）每次运行都用同一份脚本出包，打 tag 时把两个 zip 挂到 release 草稿。

## 用法

| 命令 | 作用 |
|---|---|
| `secmelt` | 命令行入口（无子命令时打印用法）；交互/点按式操作走 `secmelt-gui` |
| `secmelt-gui` | 图形前端（Dear ImGui · Win32 + D3D9）：同一套 pipeline，点按式操作，Win7 SP1 → Win11 |
| `secmelt --dump` | 自检报告：环境 + 构建期资产 + 名单的实测存在状态，只读，可用于 CI |
| `secmelt --dry-run` | 只读预演整条链路 |
| `secmelt --preflight` | melt 前的一次性根因扫描（logstate 守卫 + 目录层级探针），**只在带快照的 VM 里跑** |
| `secmelt --melt --yes-i-know` | 非交互执行整条链路；破坏性、不可回滚 |
| `secmelt --selftest-hive` | 校验 hive base block 偏移与校验和算法 |
| `secmelt --selftest-registry` | 验证过滤器摘除的写入路径 |
| `secmelt --selftest-raw` | 裸盘写入/读回判定：底层通路是硬断言，OS 通路只做分类（**只在虚拟机里跑**） |

`--selftest-raw`、`--melt`（以及图形前端的三个动作）需要管理员权限。无需担心数字签名：程序自己会搞定。

### 图形前端（secmelt-gui）

`secmelt-gui.exe` 与 `secmelt.exe` 共用同一条 pipeline（不包壳子进程），三个按钮与 CLI 一一对应：

| 按钮 | 等价命令 | 说明 |
|---|---|---|
| 预检扫描 | `--preflight` | 装驱动 + 四层探针写/读回/校验（**只在带快照的 VM 里跑**） |
| 只读预演 | `--dry-run` | 全链路预演，不写裸盘 |
| 执行 MELT | `--melt --yes-i-know` | 破坏性；落盘前弹出确认框，键入 `MELT` 才能继续 |

细节：

* **触摸优先**：控件命中盒按手指定尺寸（`FramePadding`/`CellPadding`/`TouchExtraPadding`），
  动作按钮整行可点、滚动条加宽到可直接拖；只读清单（资产表）保持紧凑行距，把纵向空间
  让给可交互区域。窗口在 150% DPI 的 1280x800 上仍然放得下。
* **配色** Catppuccin Mocha（含 1.92 新增的 `ImGuiCol_CheckboxSelectedBg` 等键位；漏一个
  就会从 `StyleColorsDark` 继承出刺眼的默认色）。
* **目标名单是动态加载的**：启动时读 `<exeDir>/targets.txt`（退回 `config/targets.txt`，
  都没有才用编译内置名单）；运行中文件一改（大小或修改时间变化，0.5s 轮询）就自动重读并
  重新探测，也可以点"重新加载名单"手动刷新。改名单不需要重启程序。
* 启动本身只做只读探测（注册表名单 + 资产清点），不装驱动、不碰盘；驱动装载与裸盘写入
  只发生在按钮点击之后。非提权时三个按钮全部禁用。任务运行期间窗口拒绝关闭。
* release 构建嵌入 `requireAdministrator`（双击即 UAC）；debug 构建嵌入 `asInvoker`，
  供宿主开发机做渲染冒烟——界面对非提权本来就做了降级，不是绕过安全检查的后门。
* 后端选 D3D9 而非 D3D11：Vista 起随系统提供，Win7 裸镜像不需要任何可再发行组件。
* 中文字形走 ImGui 1.92 的动态字形加载（不预烘焙 glyph ranges），字体按
  `%WINDIR%\Fonts` 依次尝试 msyh / Deng / simhei / simsun。

### CLI 工具的输出风格

借鉴空客A320系列/波音777/787的优良传统：如果驾驶舱里面没有什么灯亮着说明一切正常。我们同理：出错了才会有颜色。

CLI 输出按行首标记分层：`[x]` / `FAIL` / `FAILED:` → **加粗亮红**；`[!]` → 亮黄；
其余（`[*]` `[+]` …）→ 白色。两个约束：

* **重定向到文件/管道时不着色** —— 否则日志里全是转义序列。
* **只在能解析 ANSI 的终端上着色**，判定分两条路：

### 关于 --preflight 等探测选项

由于某些 SCM 的神秘 bug，在进行需要加载我们的驱动的探测环节中，在加载驱动然后卸载并清除服务键这一流程会导致SCM内部状态似乎和注册表不太一致，然后这样就会导致同一会话中第二次加载的时候报“找不到驱动文件”，解决办法：重启。


### 写回前的一致性地基检查（重要）

写回的物理位置来自 ntfscp，而 ntfscp 是按 **ntfs-3g 的视图**解析路径的。卷被还原类软件接管时，
**运行中文件系统的视图**与 ntfs-3g 读到的**基线**布局未必一致 —— 那种情况下我们会把 12 MB 的
SYSTEM hive 写到基线里属于**别的文件**的簇上。这不但毁掉那个文件，还会把症状从"缺文件"变成
随机的启动失败，比前者难查得多。

所以写回前多了一道守卫：**用 ntfs-3g 自己把目标开头 4096 字节抄出来，确认它以 ASCII `regf`
开头**（SYSTEM hive 及其日志都以此开头）。不匹配就**一个字节都不写**，直接中止 —— 此时还没过
不可回退点，能干净收场。日志检查前有同一道守卫。

为什么要绕这么一圈（`ntfs-3g-cli ... readhead` / `stat:<path>`）而不是直接用普通 API 读：

* **活动 SYSTEM hive 被内核独占持有**，按 Win32 路径 `CreateFileW` 会返回
  `ERROR_SHARING_VIOLATION(32)`（实测）。所以根本打不开它。
* 更根本的是：**要校验的就是 ntfs-3g 的视图** —— ntfscp 按它写盘，运行中文件系统的视图反而是
  不可信的那一方。

**探针用直接挂载，不用 FUSE 层**（`ntfs_mount()`，与 `ntfsfix` / `ntfscp` 同一条路），
而且**只读挂载**（`NTFS_MNT_RDONLY`）：只读不会重放日志、不会清 dirty 标记，**一个字节都不写**。
这一点很要紧 —— 之前的实现走 `ntfs_open()`（FUSE 层），它会附加 `EXCLUSIVE` /
`IGNORE_HIBERFILE`、读 `$Bitmap` 与 `$MFT` 位图、甚至处理 `hiberfil.sys`，**挂载本身就可能改卷**，
对"写之前先确认目标是什么"的探针来说是自相矛盾的。顺带也修掉了两个 bug：路径必须先过
`\`→`/` 转换（`ntfs_pathname_to_inode` 只认 `/`），以及"读不到"曾经被误诊成"内容不是 hive"。

**守卫排在 `ntfsfix` 之前**：ntfsfix 会经同一个 `handle:` 写这个卷（置 dirty 标记、修
`$MFTMirr`、清 `$LogFile`），而探针要读的是"我们动手之前"的现场。

读不到时守卫会**逐层探路径**（`/`、`/Windows`、`/Users`、…、目标），定位是哪一层断的。

名字查不到时还会**列出父目录的内容**（`list:<path>`），用于区分到底是没找到这个文件还是它根本没存在。

### 名字的大小写：**libntfs-3g 默认区分大小写**（危险）

先看源码事实（`libntfs-3g/volume.c:531`）：

```c
	/* Default with no locase table and case sensitive file names */
	vol->locase = (ntfschar*)NULL;
	NVolSetCaseSensitive(vol);      /* ← 默认区分大小写 */
```

唯一解除它的是 `ntfs_set_ignore_case()`，而**全项目只有 `src/lowntfs-3g.c:4347`（FUSE 低层
驱动）会调用它**。于是：

| 组件 | 查找是否区分大小写 |
|---|---|
| `ntfsfix` / `ntfscp` / `ntfs-3g-cli`（直接挂载） | **区分** —— 与 Windows（大小写不敏感）不一致 |
| `lowntfs-3g`（FUSE，带 ignore_case） | 不区分 |

**这个不一致是要命的**，因为 `ntfscp` 在目标查不到时会走：

```c
	out = ntfs_pathname_to_inode(vol, NULL, unix_name);
	if (!out) {
		/* Copy the file if the dest_file's parent dir can be opened. */
		...
		ni = ntfs_new_file(dir_ni, filename);   /* ← 新建一个文件 */
	}
```

所以卷上真实名字是 `system`、而我们传 `SYSTEM` 时，`ntfscp` 会在同一目录里**造出一个只有
大小写不同的重名文件**，真正的 hive 一个字节都没改，**而且它报成功**。NTFS 按设计不允许这种
状态，因此后果不止"没改到"：目录命名空间被写坏，后续 `chkdsk` / 启动修复会介入，`winload`
也再找不到它要的 hive —— 这正是"写完重启进不去 / 反复自动修复"的一条完整链路。

**所以做了三件事：**

1. 我们自己的直接挂载（只读探针与 `setdirty`）一律先 `ntfs_set_ignore_case(vol)`，行为向
   Windows 看齐；失败时不静默，会记一条错误。
2. 第 8 步先做一次 **`resolve:<path>`**（大小写不敏感回退），拿到**卷上的真实名字**，守卫、
   **`ntfscp` 的目标**、读回校验都用它 —— 这样即使 `ntfscp` 本身仍区分大小写，传进去的名字也是
   精确匹配的，不会触发 `ntfs_new_file`。
3. `resolve:` 会统计**有几个大小写不敏感匹配**；**多于一个就直接拒绝写入**：

```
[x] pre-write guard (hive name): the directory holds more than one file whose name differs only by
    case (the volume's real name resolves to \Windows\System32\config\system). NTFS does not allow
    that by design, so this volume has already been corrupted -- most likely by an earlier write that
    created a second file instead of overwriting the existing one. Refusing to write anything into it.
```

名字与预期不一致时会明确报出来：

```
[!] the hive's name on this volume is \Windows\System32\config\system, not
    \Windows\System32\config\SYSTEM (ntfs-3g's exact lookup fails; using the real name for both
    the guard and the write)
```

顺带一提，`--selftest-raw` **证明不了这一条**：它只证明"区段可写、写进去能读回"，用的是它自建
的文件。守卫问的是另一个问题 —— *这个路径在基线磁盘上究竟装着什么*。

写 + 校验最多两轮：`ntfscp` 报 Done、长度也对，读回来的字节却不一致时（实测出现过），会
**重写一次再校验**（内容相同、幂等，所以重试无害），而不是把一个只写了一半的 hive 留在启动
路径上。

读回不符时会给出**差异统计**，因为"丢一个字节"、"整片区域是零"、"整段是旧内容"是完全不同的成因：

```
[!] the hive ntfs-3g reads back differs from the export: <N> differing byte(s) out of <M>;
    first at <off> (disk 0xNN, file 0xNN), last at <off>; <K> of them are 0x00 on disk;
    disk length <A> vs export <B>
```

并且**再读一次**做对照，用来区分两种根本不同的情况：

| 两次读回 | 结论 |
|---|---|
| **一致** | 卷是稳定的 → 差异是"写没写全"（工具/驱动/连接层） |
| **不同** | 有东西正在改这个卷 |

```
[!] the volume is stable between two consecutive read-backs, so the difference is a write that
    did not land in full (the tool reported success but the bytes are not on disk)
[!] the volume is CHANGING between reads: <N> byte(s) differ ... -- something is rewriting this
    volume while we work
```

卷稳定时**再往下挖一层**：写路径本身行不行？**同一份导出、同样大小、同一个目录**，只改目标名字，
写两遍做对照：

| 组 | 目标 | 目的 |
|---|---|---|
| 1 | 4 MiB 图案 → 卷根**新文件** | 最小往返检查 |
| 2 | **同样的 11.6 MiB 导出** → `config\secmelt-probe.hive`（**新名字，内核没持有**） | 把"大小/耗时"与"内核正持有那个文件"分开 |

判读：

| 结果 | 结论 |
|---|---|
| 第 2 组**干净往返** | 大小/耗时不是原因；**内核正持有那个文件**才是 —— 它的缓存元数据/数据页被写回，盖掉我们的裸写。**在运行中的系统上原地重写活 hive 这条路走不通** |
| 第 2 组**也脏** | 与"那个特定文件"无关；是大小/耗时或写本身 |

试完两个探针文件都会**删掉**（`rm:`，我们自己建的）。

（更早那版用"按运行中文件系统的区段经驱动读回来比" —— **那个设计不成立**：`readhead` 读的是同一份
磁盘 `$MFT` 给出的同一批簇，比出来必然一致。而且它用 Win32 路径取区段，对活动 hive 直接
`ERROR_SHARING_VIOLATION(32)` 失败 —— 同一个坑踩了第二次。）

守则会放行 **0 字节的目标**：0 字节不是"未知内容"，是"什么都没有"，覆盖它不会毁掉任何东西，
反而是修复 —— 实测这台机器的 `config\RegBack\SYSTEM` 就是 0 字节，把有效 hive 放回那个位置
正是我们想要的。顺带一提，守卫报的 base block 里的 `file size` / `rootCellValid` 现在取自
readhead 报告的**真实文件长度** —— 只读前 4096 字节时文件长度未知，`rootCellValid` 会给出
`no` 这种**误导性**结论（踩过一次）。

### 不自动复位：写完自己重启

`--melt --yes-i-know` 与图形前端的 Melt 写完、逐字节校验、置好 dirty 标记之后就**结束**了 ——
不 bugcheck、没有任何自动复位路径，只打印结论并提示你重启：

```
[+] all writes are complete and verified
[!] handing the reboot back to you: nothing is reset automatically. Restart the machine yourself when you are ready -- a normal reboot applies this change.
```

理由有两个，都指向同一件事：

* 改动是在**基线**（裸盘）上的，按一次**普通重启**就能生效 —— 不需要用 bugcheck 去"抢在注册表
  懒写回之前复位"（那个理由在冻结的机器上不成立，见下节）；
* bugcheck 会在你读完日志、用别的工具核对现场之前把机器打下去。

早期版本在 CLI 模式下读一次回车就 `KeBugCheckEx()`，交互模式更是写完就自动打下去。整条 bugcheck 路径**已经移除**：驱动侧的 `CTL_REBOOT_SYSTEM`（`KeBugCheckEx`）与用户态的`RebootNow`、以及那段"按回车触发蓝屏"的提示全部删掉了 —— 任何入口都不会复位这台机器。毕竟考虑到 Windows 已经不再可能写入我们的磁盘（被重定向了）这么暴力的做法已经没有必要了（这里关于 KeBugCheckEx 有一个彩蛋，各位可以去 git 历史里面找找）

### 不进内核也能查：用设备名直接读（不需要驱动）

`ntfs-3g-cli` 的一次性命令接受任意设备名，其中 `"C:"` 形式走的是低层卷访问
（`\??\C:` + `FSCTL_LOCK_VOLUME`，只锁不写），**不需要 WinDisk 驱动、也不需要 DSE**：

```
ntfs-3g-cli.exe "\\.\C:" list:/Windows/System32/config
ntfs-3g-cli.exe "\\.\C:" resolve:/Windows/System32/config/SYSTEM
ntfs-3g-cli.exe "\\.\C:" stat:/Windows/System32/config/system
ntfs-3g-cli.exe "\\.\C:" readhead /Windows/System32/config/system D:\head.bin 4096
```

`readhead` 的**后两个参数两种顺序都接受**（哪个是纯数字就当字节数）。

这几条**全是只读的**（`NTFS_MNT_RDONLY`，不重放日志、不清 dirty 标记、一个字节都不写），所以可以在**起不来的机器**上用 WinRE 的命令行跑，直接看清卷上到底有什么、真实名字是什么。

### 先停服务，再删服务键

**只删除注册表键并不会停止已经加载的驱动。** 所以对每个命中的目标服务，第 5 步会：

1. **先停**（`ControlService(SERVICE_CONTROL_STOP)`，最多等 2 秒确认真的到 `SERVICE_STOPPED`）；
2. 再删注册表键。

**停不下来不算失败**，这是刻意的：内核过滤器驱动大多没有卸载例程，`ControlService` 会直接返回
`1061`（`ERROR_SERVICE_CANNOT_ACCEPT_CTRL`）。那不影响最终效果 —— 键删掉之后重启就不再加载 ——但**必须如实报出来**，因为"现在它还在跑"意味着**到重启前保护依然生效**：

```
[+] stopped service: DFServ (stopped)
[!] service NOT stopped: DeepFrz (still running (ControlService(SERVICE_CONTROL_STOP) failed: 1061,
    the service does not accept control commands)) -- it stays active until the next reboot
```

注册表键**不存在时也会问一次 SCM**：SCM 的内存记录可以比注册表键活得更久（键被删了、服务还在跑），只看注册表就会漏掉一个**正在生效**的过滤器：

```
[!] service NOT stopped: DeepFrz (still running ...; its registry key is already gone, so nothing
    points at this driver any more -- expect it to disappear after the reboot)
```

`--dry-run` 里这一步是**只查询不控制**（`QueryServiceRunning`，只有 `SERVICE_QUERY_STATUS`），毕竟，预演绝不能真的去 `ControlService`，那就不叫只读预演了：

```
[!] would stop and delete service key: HKLM\SYSTEM\CurrentControlSet\Services\DeepFrz [RUNNING (state 4)]
```

## 拦截模型的实证：sector 层只影子引导区，"还原"发生在卷/文件层

对 DeepFreeze 8.63 的三枚驱动做了全量逆向（Ghidra headless 全函数反编译），并在冻结的Win7 与 Win10 上做了 A/B 实测（完全不走任何旁路、只走普通 `IoCallDriver` 的版本，melt照常落盘），拦截模型如下：

| 层 | 组件 | 干什么 | 对本工具的影响 |
|---|---|---|---|
| sector 层（端口驱动 dispatch hook） | DfDiskLo.sys | **只影子引导结构**：MBR 盘 = LBA0；GPT 盘 = LBA0 + GPT 头 + 分区表数组，copy-on-write 进内存（RAM-only，每 boot 重建）| **无** —— 数据区读写被它的 stub 原样转交原始 dispatch，hive/RegBack/dirty 标记的路径上没有 sector 级拦截 |
| 卷/文件层 | DeepFrz.sys | 真正的"还原"：普通写 COW 进它的隐藏磁盘存储，重启丢弃；**没有 SRB 能力** | **无** —— 端口层裸写在它的视野之外，绕它不靠任何钩子 |
| 文件系统层 | DFFilter.sys | 观察 + 机器账户保护 | 无 |

推论：**本工具不需要绕过任何钩子**。WinDisk 的 SRB 天然从还原软件的视野下面穿过 ——早期版本曾实现过一套从 hooker 内存记录里"救回原始 dispatch"的 bypass，上述实测证明它对 hive 写入多余，已经整体删除（留档于 git 历史）。

**唯一的边界**：别碰 MBR/GPT 分区表几个扇区 —— 那是唯一被 sector 影子覆盖的区域，写它会被内存影子吞掉（会话内读回自洽、重启蒸发）。本工具不碰它们（也正因如此，那些"必须改 MBR才能杀冰点"的传说与本工具无关）。

## 原理：为什么"写坏也炸不了机"

**我们要的是 patch hive 里的 filter 清单 → 写到裸磁盘 → 重启后 Windows 不再加载那些过滤驱动。**改动是给**下一次启动**用的，不需要在当前会话里生效。

关键推论 —— **我们的基线写入不可能被 Windows 覆盖**：Windows 自己的任何写入都走文件系统 → 卷 → 磁盘栈，而还原软件在**卷/文件层**把那些写入 COW 进增量区（见上一节的拦截模型），到不了基线。所以：

* 内核的内存注册表懒写回 → 被重定向 → **盖不到我们写上去的 hive**；
* 因此 **bugcheck 不是必需的**：它存在的理由（防止内存里的注册表刷回磁盘覆盖我们写的 hive）
  在冻结的机器上不成立。按一次**普通重启**同样能让改动生效；
* 也正因如此，**"读回校验没通过"不等于任务失败** —— 它只是证据，不是停止信号。

于是写回这一段的行为是：

| 情形 | 行为 |
|---|---|
| 回滚镜像取不到 | 只告警，**继续**（读路径的问题不该阻止写基线） |
| 预演（同字节写到临时名字）没通过 | 只报告，**继续**写真 SYSTEM |
| 写后读回不一致 | 只报告 + 保留回滚镜像的路径，**不自动回滚、不中止** |
| 完成后 | 打印结论并提示**自行普通重启**（本工具不做任何自动复位） |

**为什么"写回不一致"和"能不能开机"是两件事**：磁盘上留下的是一份读回不一致的 hive —— 引导器只能去读它；而内核内存里那份注册表仍然是**可用**的替代品。所以**读回不一致时不要 bugcheck**（本版已经没有任何自动复位路径）：普通重启是安全的（不会丢内存注册表），而"重启后过滤驱动是否
还在加载"才是"写到底有没有落上"的真正答案。

## NTFS 相关杂物

### 读回"一大片连续的 0x00"意味着什么（`info:<path>`）

NTFS 的属性有**两个**长度字段，而**读到 `initialized_size` 之外返回零**；runlist 里的 **hole
（LCN == -1）同样返回零**。所以"读回来有一大段连续的零"有两种可能，光比内容永远分不清：

| 现象 | 成因 |
|---|---|
| 磁盘上 `initialized_size < data_size` | 数据没被写到"已初始化"区之外 → 那一段按 NTFS 语义就是零 |
| runlist 里有 **hole** | 洞按 NTFS 语义就是零 |
| 整段是**旧内容** | 按磁盘上的映射读到了**别的簇**（映射没更新 / 分配到了别处） |

所以新增了 `info:<path>`（只读直接挂载），把**磁盘上那个 inode** 的关键字段与 runlist 打印出来：

```
data_size=12111872 initialized_size=... compressed_size=...
resident=0 flags(sparse=0 compressed=0 encrypted=0)
runlist: N run(s), M hole run(s), K cluster(s) allocated (cluster=4096)
  vcn=0        lcn=123456     len=100
  vcn=100      lcn=-1         len=572   <-- HOLE (reads as zeros)
  ...
```

`ntfscp` 在写回失败时**会自动把它打进日志**（`on-disk inode metadata for the target:`），所以下一次
复现就自带答案，不用再猜。

顺带记两个从源码里读到的前提（都在 vendored 的 ntfs-3g 里）：

* `NTFS_BUF_SIZE = 8192` —— `ntfscp` 对 12 MB 文件要调 **1478 次** `ntfs_attr_pwrite`；
* `attrib.c` 在处理"写跨越 `initialized_size`"时会**把空隙补零**（`ntfs_attr_fill_zero`）并把
  `initialized_size` 推到 `pos + count`。所以有两个长度字段可能不一致。

另外确认了一件事：`ntfscp` 的 `-f` 是 `--force`，`-m` 才是 `--minfragments`；我们传的是 `-f -v`，
所以走的是 `ntfs_attr_truncate_solid()`（HOLES_NO），**不是**那条自研 runlist 分配
（`find_best_runs` / `assign_runlist`）的路径 —— 那条路会直接改属性的 runlist 而不写数据，
是另一个值得警惕的坑，但我们没走。

> 实际上这个功能是额外的。早期出现过一个乱七八糟的情况，那就是写入的时候老是只写一点点就没了（类似于下面几行的那个4701），至于根本原因，后面会讲。

### `record:<path>`：MFT 记录法证倾倒（谁最后写了这条记录）

当 `initialized_size` 出现一个**任何合法写入者都产生不了的值**（例如 4701 —— 拷贝循环只会写`0` 或 `8192` 的倍数，truncate 只写 `0`/`newsize`/对齐值）时，解析后的字段已经不够用了，只有**字节本身**能定案。`record:<path>`（只读直接挂载）把那条 MFT 记录倾倒出来：

* 记录头（magic/序号/链接数/`bytes_in_use`/`base_record_ref`）；
* 逐属性布局：类型、长度、驻留与否、非驻留的 `alloc/data/init` 三个原始长度字段；
* `$STANDARD_INFORMATION` 的**四个时间戳** —— 那是"最后写入者"的笔迹（ntfs-3g 关路径与
  Windows 内核/还原软件写的值与字段次序都不同）；
* 头 256 字节十六进制倾倒（布局错位/半新半旧肉眼可辨）。

读回校验失败与大小二分的每一档都会自动把它带进日志，无需手动跑。

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

只放注册项名（过滤器项名 / 服务键名），不放文件路径。注意一下编码问题，如果输出是乱码就换个编码（UTF8/GBK都可以试试看）

## FAQ

### 运行时

**支持哪些系统？有什么前提条件？**
Windows 7 SP1（要带 SHA-2 补丁）可以直接用。Windows 10 / 11 这类较新的系统，必须先关掉VBS 与 HVCI（内存完整性）—— 在「Windows 安全中心 → 设备安全性 → 内核隔离」里可以一并关闭，这是这一切实施的必要条件。同时关掉其它安全软件与 Windows Defender 的实时保护（病毒和威胁防护里）。

**启动修复报 `BadPatch` / "A patch is preventing the system from starting"？**
这是启动修复对「补丁状态不完整 / 注册表被离线改过一半」的诊断签名，它的修复动作通常是**系统还原**。
我们的 melt 恰好就是一次离线改注册表，因此要保证**改得"整"**：

**`--selftest-raw` 报 `FAIL uncached FS read-back mismatch at byte 0: 0xA5`？**
先分清那条读回走的是哪条路：

| 报告行 | 通路 | 判据 |
|---|---|---|
| `raw pre-read` / `raw read-back` | WinDisk 驱动 → 磁盘 miniport | 硬断言：写必须真的落到那些物理偏移上 |
| `OS read-back (file-system path, uncached)` | 文件系统（`FILE_FLAG_NO_BUFFERING` 只绕缓存，**不绕过滤器**） | 分类，不是断言 |

机器处于冻结状态（冰点/影子系统这类卷过滤器在文件系统之下截住写）时，两条路**必然**
不一致：驱动能把磁盘改成 B，文件系统仍然看到 A。这正是过滤器在工作的证据 —— 也是
melt 依赖的"绕过上层"通道。所以现在 OS 侧读到 A 会被判定为 `a volume filter is holding
the original blocks`，只有"既不是 A 也不是 B"才报 FAIL。

**环境屏说 `Source tree ... not present (deployed bundle)`？**
那是正常的。`third_party/`、`config/` 那些是**构建期**资产；把它们拷到目标机的
release 目录里没有它们，运行时用的也不是它们（运行时只用 exe 旁边的
`WinDisk_x64.sys` / `kdu.exe` / `drv64.dll` / `targets.txt` / `tools`）。源码树不在时
只报这一行，而不是刷 6 条"缺失"故障。

**`--melt` 跑完重启后进不去系统（`0xC0000225` = INACCESSIBLE_BOOT_DEVICE）？**
先分清是**哪一环**出的问题 —— 症状一样，责任方完全不同。按可能性排序：

1. **磁盘上那份 hive 没落对**（最该先排除）。第 8 步的读回校验会直接回答：
   * 报 `the hive ntfs-3g reads back differs from the export at byte N` → 写到位了但不是我们的内容；
   * 报 `the base block on disk is not a clean, checksum-valid hive` → 内核会当它脏，于是去
     尝试重放日志；日志我们没动过，但对我们这份新 hive 也不可应用 → 系统 hive 加载失败；
   * 报 `ntfs-3g reads back the exact hive we wrote, and its base block is clean and
     checksum-valid` → hive 这一环是干净的，去下面找原因。若守卫更早就报
     `ntfs-3g does not see a hive here: first bytes are ...`，那说明连写都没写（安全中止）。
2. **基线（被冻结保护的那份）与当前状态不一致**。冻结点之后的改动（**新建的文件**尤其）存在于还原软件的增量区里，而我们的裸写只把当前 hive 写进了基线：于是注册表引用的某个文件在基线磁盘上**并不存在**。若它是启动链上的（BOOT_START 驱动之类），症状正是 0xC0000225。这类失败与 hive/日志无关，读回校验会显示一切正常。
   验证办法：别修 hive，直接按还原产品自己的流程解除保护/提交增量后再启动一次做对照。
3. **摘掉的过滤项里有启动必需的**。代码只删命中名单的项、非空结果绝不整值删除（`partmgr`/`kbdclass`/`mouclass` 这类系统自身项会保留），`--selftest-registry` 就是验证这条写入路径的。若怀疑，用 `--dry-run` 看它**打算**删什么（只读探测，不写）。
4. **还原软件有启动期组件**（MBR/引导扇区上另装一层）。这种情况下它在 Windows 之前就把基线改回去了，hive 与日志都无能为力。（常见例子: Rollback RX Pro，但是这个东西的支持还需要我多实验看看，目前请不要用）

**HKCU 的 hive 导出报 `1314`？**
`RegSaveKeyEx` 需要 `SE_BACKUP_NAME`，即便导出的是自己账户的 hive —— 所以 `--selftest-hive`也要提权（这也是为什么它不能在普通命令行里跑）。

**重启后没看到磁盘检查 / 起不来（`0xC0000225`）？**
先看日志里这几行：`the system volume is marked dirty`、`base block on disk: ... clean=yes checksumOk=yes`。

* 置标记**成功 + 读回校验通过**，却仍起不来 → hive 这一环是干净的，问题多半在**基线**：
  冻结点之后新建的文件只存在于还原软件的增量区，我们把当前 hive 写进基线后，注册表引用的
  某个文件在基线上并不存在（若是启动链上的驱动，症状正是"需要的设备不可访问"）。对于这类 bootkit 建议先排查一下。一个白板 Windows 是什么样的，随便问一个ai就知道；请务必记得你在当前会话装了啥软件。

**没有交互界面了？**
CLI 只做命令行工具：每条子命令跑完打印报告就退出，不依赖任何终端能力 —— Windows 7 的原生
conhost 也能完整工作。需要点按式操作时用图形前端 `secmelt-gui`（同一份 pipeline）。

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
`KDU`（MIT，submodule）、`WinDisk`（本项目）、`Dear ImGui`（MIT），均与 GPLv3 兼容。

## 免责声明

涉及内核驱动加载与裸盘写入，误用可导致数据损坏或系统无法启动。仅在你有权操作的机器与磁盘上
使用。第 8 步之后一旦开始写入就没有回滚路径：出错时程序会中止并报出"系统状态未知"，然后提示
你自行重启（本工具不触发任何复位）。

## 后记

丙午年农历八月初二，项目业已落地。循其本，乃发于 2025，亦可朔至 2024 年八月。期间，承蒙各路大神指点，历经波折，或行时停，亦有巧思，亦遇困难，所幸始得 AI 之力，项目方成。

忆当初，以此发家，理论研究，亦有成果，可惜未成气候，也未出成品，以愚不擅 ui 编程，遂搁置。

几周之前，偶知 ftxui，眼前一亮，跃跃欲试，亦成气候，颇为顺手，遂得动力，重拾旧业，继续开发。高一学业繁忙，只得抽出暇余，断断续续，勉力为之，今日成也。

燕雀安知鸿鹄之志哉？余之志，不在高中一“多媒体管理员”，每日浑浑噩噩，为讲台所绊，为琐事所扰，碌碌无为，虚度光阴。余之志，在星辰大海！底层开发，操作系统，人工智能，网络安全。遂入软件开发之业，苦练竞赛，研工程之技巧，领算法之精髓，攀技术之高峰，潜底层之深海。三年磨一剑，以此，纪念我初中未竟之梦，破开束缚电教自由操作的厚厚坚冰。

也愿，以我之毅力，我之勇气，我之决心，破开附着于我心中的，那层坚冰。破茧成蝶，融冰成水，IceMelt，由此得名。

chenjintang-shrimp/沧粟虾，
2026年9月12日
