#pragma once

/*
 * POSIX types (mode_t, off_t, dev_t, uid_t, gid_t) come from the platform:
 * Cygwin's <sys/types.h> has all of them, and the mingw-w64 build gets
 * mode_t/off_t/dev_t from <sys/types.h> plus uid_t/gid_t from the shim in
 * mingw-compat/. The previous hardcoded `typedef unsigned int mode_t;` block
 * was only ever dead code under Cygwin (its headers define _TYPES_DEFINED) and
 * collides with mingw's `typedef _mode_t mode_t` (unsigned short).
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_UTIME
#include <utime.h>
#endif

typedef int(*fuse_fill_dir_t)(void *, char *, struct stat *, int);

#ifdef __cplusplus
extern "C" {
#endif

int ntfs_fuse_readlink(const char *org_path, char *buf, size_t buf_size);
int ntfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int ntfs_fuse_open(const char *org_path);
int ntfs_fuse_read(const char *org_path, char *buf, size_t size, off_t offset);
int ntfs_fuse_write(const char *org_path, const char *buf, size_t size, off_t offset);
int ntfs_fuse_truncate(const char *org_path, off_t size);
int ntfs_fuse_ftruncate(const char *org_path, off_t size);
int ntfs_fuse_chmod(const char *path, mode_t mode);
int ntfs_fuse_chown(const char *path, uid_t uid, gid_t gid);
int ntfs_fuse_create(const char *org_path, mode_t typemode, dev_t dev, const char *target);
int ntfs_fuse_mknod(const char *path, mode_t mode, dev_t dev);
int ntfs_fuse_create_file(const char *path, mode_t mode);
int ntfs_fuse_symlink(const char *to, const char *from);
int ntfs_fuse_link(const char *old_path, const char *new_path);
int ntfs_fuse_rm(const char *org_path);
int ntfs_fuse_unlink(const char *org_path);
int ntfs_fuse_rename_existing_dest(const char *old_path, const char *new_path);
int ntfs_fuse_rename(const char *old_path, const char *new_path);
int ntfs_fuse_mkdir(const char *path, mode_t mode);
int ntfs_fuse_rmdir(const char *path);
#ifdef HAVE_UTIME
int ntfs_fuse_utime(const char *path, struct utimbuf *buf);
#endif
int ntfs_fuse_fsync(const char *path, int type);
int ntfs_fuse_bmap(const char *path, size_t blocksize, uint64_t *idx);
void ntfs_close(void);
int ntfs_fuse_init(void);
int ntfs_open(const char *device, BOOL ro, BOOL exclusive);
int ntfs_fuse_stat(const char *org_path, struct stat *sbuf);

/* ---- 直接挂载的探针/写入（不经过 FUSE 层）---------------------------------
 *
 * ntfsfix / ntfscp 用的就是这条路（utils_mount_volume → ntfs_mount），它在 handle: 卷上
 * 已被证明可用。FUSE 层（ntfs_open）会附加一堆额外行为 —— EXCLUSIVE、IGNORE_HIBERFILE、
 * $Bitmap / $MFT 位图读取、以及对 hiberfil.sys 的处理 —— 对"只读看一下这个路径装的是
 * 什么"来说既没必要，还多出改动卷的机会。
 *
 * 只读探针一律用 NTFS_MNT_RDONLY 挂载：只读不会重放日志、不会清 dirty 标记，**一个字节
 * 都不写**。返回值 <0 表示 -errno。 */
int ntfs_read_file_direct(const char *device, const char *path, char *buf, size_t size,
                          long long *total_size);
/* 一次挂载里逐个解析多个路径（供"读不到时逐层定位"用）；sizes[i] < 0 表示该层不存在。
 * 返回成功解析到的层数，<0 表示 -errno（挂载就失败了）。 */
int ntfs_stat_paths_direct(const char *device, const char *const *paths, int count,
                           long long *sizes);
/* 置卷的 VOLUME_IS_DIRTY 标记（chkdsk /f 对在用卷做的事）。需要写权限，因此用
 * NTFS_MNT_RECOVER 挂载。返回 0 成功、<0 表示 -errno。 */
int ntfs_set_volume_dirty_direct(const char *device, unsigned short *flags_before,
                                 unsigned short *flags_after);
/* 列一个目录（只读直接挂载），每行 "<D|F>\t<name>"。返回条目数，<0 表示 -errno。 */
int ntfs_list_dir_direct(const char *device, const char *path, char *buf, size_t cap);
/* 删一个文件（可写直接挂载）。返回 0 成功、<0 表示 -errno。 */
int ntfs_delete_direct(const char *device, const char *path);
/* 把磁盘上那个 inode 的关键元数据与 runlist 打出来（只读直接挂载）。
 *
 * 为什么需要：NTFS 的属性有 **data_size**（文件长度）和 **initialized_size**（已初始化长度）两个
 * 字段，而**读到 initialized_size 之外会返回零**；runlist 里的 **hole（LCN == -1）同样返回零**。
 * 所以"读回来有一大片连续的 0x00"可能是这两种之一 —— 必须看磁盘上的元数据才能定案，
 * 光比内容永远分不清"写没落上"和"落在了一个洞/未初始化区里"。
 *
 * 返回 0 成功、<0 表示 -errno；成功时 out 里是多行文本。 */
int ntfs_attr_info_direct(const char *device, const char *path, char *out, size_t cap);
int ntfs_resolve_path_direct(const char *device, const char *path, char *out, size_t cap);
/* 把磁盘上那个 inode 的 MFT 记录原始布局/字节倾倒出来（只读直接挂载）。
 * initialized_size 出现"任何合法写入者都产生不了的值"时，用字节本身定案：
 * 属性布局、序号、$STANDARD_INFORMATION 时间戳（最后写入者的笔迹）。 */
int ntfs_record_dump_direct(const char *device, const char *path, char *out, size_t cap);
/* 只读探测 $LogFile 重启页版本与卷 dirty 位（ntfs-3g-cli ... logstate）。
 * RW 挂载在重启页 v2.0 时被无条件拒绝（"Windows 持有缓存元数据"）——把这个
 * 状态在写回之前显式报出来，操作者先真关机再重试，而不是让 ntfscp 摔死。 */
int ntfs_logstate_direct(const char *device, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
