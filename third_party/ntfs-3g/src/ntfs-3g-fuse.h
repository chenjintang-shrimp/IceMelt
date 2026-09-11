#pragma once
#include <sys/stat.h>

#ifndef _TYPES_DEFINED
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

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

#ifdef __cplusplus
}
#endif
