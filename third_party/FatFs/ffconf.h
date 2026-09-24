/*----------------------------------------------------------------------------/
/  FatFs - Configuration for IceMelt                                           /
/-----------------------------------------------------------------------------/
/ 这份配置是给 IceMelt 的"裸扇区写 ESP"这条路径用的，取舍都写在这里：
/
/   * FF_FS_READONLY 0 / FF_USE_MKFS 1 —— 要写文件；自检里还要用 f_mkfs 造合成镜像
/     （这样自检不再需要自己实现 FAT32 格式化）。
/   * FF_MIN_SS == FF_MAX_SS == 512 —— 只支持 512 字节扇区。ESP 恒为 512，而且固定扇区
/     尺寸能关掉 FatFs 的"逻辑扇区 ≠ 物理扇区"换算路径，少一层可能出错的换算。
/   * FF_USE_LFN 1 + FF_LFN_UNICODE 1 —— 打开 LFN（真实 ESP 上 \EFI\Microsoft 就是长名，
/     startup.nsh 也超过 8.3）。1 = 用 BSS 上的静态工作缓冲，**不用 malloc**。
/   * FF_FS_TINY 0 —— 每个文件对象自带窗口（512 B），整扇区读写直接下发，不做 malloc。
/   * FF_FS_LOCK 0 / FF_FS_REENTRANT 0 —— 单线程 CLI，不需要锁表与重入。
/   * FF_FS_NORTC 0 —— 要提供 get_fattime()（在 raw/fat_volume.cpp 里，用 Win32 本地时间）。
/   * FF_FS_NOFSINFO 1 —— 空闲簇数**不信** FSInfo 里的缓存值，每次现扫 FAT。部署前要用它
/     跟"这次要写多少字节"作比较，宁可多花一次扫 FAT（500 MB 的 ESP 上是几千次 512 B 读）
/     也不要拿别人的陈旧数字做判断。分配仍然用 FSInfo 的"下一个空闲簇"提示。
/   * FF_VOLUMES 4 —— diskio 层按盘号分槽；同时挂载通常只有 1 个，留几个给自检。
/   * 关掉用不到的：find/expand/chmod/label/forward/strfunc/trim/EXFAT/64bit LBA。
/     其中 FF_LBA64 0 表示 LBA_t 是 32 位扇区号 —— 2 TB 以内足够（ESP 是 500 MB 级），
/     而且调用方（raw/fat_volume.cpp）要自己把 LBA 换算成字节偏移，越简单越好。
/
/  注意：include/ 里的 ff.c 与 ffunicode.c 是**上游原文**（R0.15 w/patch1），不要改；
/  所有取舍只写在本文件与 raw/fat_volume.cpp 里，方便日后升级对齐。
/---------------------------------------------------------------------------*/

// ff.h 会核对这个值（R0.15 w/patch1 = 80286）：改配置时若与 ff.c 版本不符，
// 编译期就报 "Wrong configuration file"，避免运行期出现难查的错位。
#define FFCONF_DEF	80286

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		1
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3
#define FF_CODE_PAGE	437
#define FF_USE_LFN		1
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	1
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		2
#define FF_VOLUMES		4
#define FF_STR_VOLUME_ID	0
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0
#define FF_FS_TINY		0
#define FF_FS_EXFAT		0
#define FF_FS_NORTC		0
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2024
#define FF_FS_NOFSINFO	1
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
