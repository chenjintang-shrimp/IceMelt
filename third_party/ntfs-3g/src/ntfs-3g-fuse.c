/**
 * ntfs-3g - Third Generation NTFS Driver
 *
 * Copyright (c) 2005-2007 Yura Pakhuchiy
 * Copyright (c) 2005 Yuval Fledel
 * Copyright (c) 2006-2009 Szabolcs Szakacsits
 * Copyright (c) 2007-2021 Jean-Pierre Andre
 * Copyright (c) 2009 Erik Larsson
 *
 * This file is originated from the Linux-NTFS project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (in the main directory of the NTFS-3G
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <locale.h>
#include <limits.h>
#include <sys/stat.h>
#include <utime.h>

#include "compat.h"
#include "attrib.h"
#include "inode.h"
#include "volume.h"
#include "dir.h"
#include "unistr.h"
#include "layout.h"
#include "index.h"
#include "ntfstime.h"
#include "security.h"
#include "reparse.h"
#include "ea.h"
#include "object_id.h"
#include "efs.h"
#include "logging.h"
#include "xattrs.h"
#include "misc.h"
#include "ioctl.h"
#include "ntfs-3g-fuse.h"

typedef enum {
	NF_STREAMS_INTERFACE_NONE,		/* No access to named data streams. */
	NF_STREAMS_INTERFACE_XATTR,		/* Map named data streams to xattrs. */
	NF_STREAMS_INTERFACE_WINDOWS,	/* "file:stream" interface. */
} ntfs_fuse_streams_interface;

typedef struct {
	ntfs_volume *vol;
	unsigned int uid;
	unsigned int gid;
	unsigned int fmask;
	unsigned int dmask;
	ntfs_fuse_streams_interface streams;
	BOOL ro;
	BOOL windows_names;
	BOOL compression;
	BOOL recover;
	BOOL hiberfile;
	BOOL exclusive;
} ntfs_fuse_context_t;

#define set_archive(ni) (ni)->flags |= FILE_ATTR_ARCHIVE

typedef struct {
	fuse_fill_dir_t filler;
	void *buf;
} ntfs_fuse_fill_context_t;

enum {
	CLOSE_COMPRESSED = 1,
	CLOSE_ENCRYPTED = 2,
	CLOSE_DMTIME = 4,
	CLOSE_REPARSE = 8
};

static ntfs_fuse_context_t *ctx;
static u32 ntfs_sequence;

static const char ntfs_bad_reparse[] = "unsupported reparse tag 0x%08lx";
	 /* exact length of target text, without the terminator */
#define ntfs_bad_reparse_lth (sizeof(ntfs_bad_reparse) + 2)

/**
 * ntfs_fuse_is_named_data_stream - check path to be to named data stream
 * @path:	path to check
 *
 * Returns 1 if path is to named data stream or 0 otherwise.
 */
static int ntfs_fuse_is_named_data_stream(const char *path)
{
	if (strchr(path, ':') && ctx->streams == NF_STREAMS_INTERFACE_WINDOWS)
		return 1;
	return 0;
}

static void ntfs_fuse_update_times(ntfs_inode *ni, ntfs_time_update_flags mask)
{
	ntfs_inode_update_times(ni, mask);
}

static s64 ntfs_get_nr_free_mft_records(ntfs_volume *vol)
{
	ntfs_attr *na = vol->mftbmp_na;
	s64 nr_free = ntfs_attr_get_free_bits(na);

	if (nr_free >= 0)
		nr_free += (na->allocated_size - na->data_size) << 3;
	return nr_free;
}

/**
 * ntfs_fuse_parse_path - split path to path and stream name.
 * @org_path:		path to split
 * @path:		pointer to buffer in which parsed path saved
 * @stream_name:	pointer to buffer where stream name in unicode saved
 *
 * This function allocates buffers for @*path and @*stream, user must free them
 * after use.
 *
 * Return values:
 *	<0	Error occurred, return -errno;
 *	 0	No stream name, @*stream is not allocated and set to AT_UNNAMED.
 *	>0	Stream name length in unicode characters.
 */
static int ntfs_fuse_parse_path(const char *org_path, char **path,
		ntfschar **stream_name)
{
	char *stream_name_mbs;
	int res;

	stream_name_mbs = strdup(org_path);
	if (!stream_name_mbs)
		return -errno;
	if (ctx->streams == NF_STREAMS_INTERFACE_WINDOWS) {
		*path = strsep(&stream_name_mbs, ":");
		if (stream_name_mbs) {
			*stream_name = NULL;
			res = ntfs_mbstoucs(stream_name_mbs, stream_name);
			if (res < 0) {
				free(*path);
				*path = NULL;
				return -errno;
			}
			return res;
		}
	} else
		*path = stream_name_mbs;
	*stream_name = AT_UNNAMED;
	return 0;
}

static void set_fuse_error(int *err)
{
	if (!*err)
		*err = -errno;
}

int ntfs_fuse_readlink(const char *org_path, char *buf, size_t buf_size)
{
	char *path = NULL;
	ntfschar *stream_name;
	ntfs_inode *ni = NULL;
	ntfs_attr *na = NULL;
	INTX_FILE *intx_file = NULL;
	int stream_name_len, res = 0;
	REPARSE_POINT *reparse;
	le32 tag;
	int lth;

	/* Get inode. */
	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	if (stream_name_len > 0) {
		res = -EINVAL;
		goto exit;
	}
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
		/*
		 * Reparse point : analyze as a junction point
		 */
	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	res = -ENOTSUP;
	//	goto exit;
	//}
	/* Sanity checks. */
	if (!(ni->flags & FILE_ATTR_SYSTEM)) {
		res = -EINVAL;
		goto exit;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		res = -errno;
		goto exit;
	}
	if ((size_t)na->data_size <= sizeof(INTX_FILE_TYPES)) {
		res = -EINVAL;
		goto exit;
	}
	if ((size_t)na->data_size > sizeof(INTX_FILE_TYPES) +
			sizeof(ntfschar) * PATH_MAX) {
		res = -ENAMETOOLONG;
		goto exit;
	}
	/* Receive file content. */
	intx_file = ntfs_malloc(na->data_size);
	if (!intx_file) {
		res = -errno;
		goto exit;
	}
	if (ntfs_attr_pread(na, 0, na->data_size, intx_file) != na->data_size) {
		res = -errno;
		goto exit;
	}
	/* Sanity check. */
	if (intx_file->magic != INTX_SYMBOLIC_LINK) {
		res = -EINVAL;
		goto exit;
	}
	/* Convert link from unicode to local encoding. */
	if (ntfs_ucstombs(intx_file->target, (na->data_size -
			offsetof(INTX_FILE, target)) / sizeof(ntfschar),
			&buf, buf_size) < 0) {
		res = -errno;
		goto exit;
	}
exit:
	if (intx_file)
		free(intx_file);
	if (na)
		ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

static int ntfs_fuse_filler(ntfs_fuse_fill_context_t *fill_ctx,
		const ntfschar *name, const int name_len, const int name_type,
		const s64 pos __attribute__((unused)), const MFT_REF mref,
		const unsigned dt_type __attribute__((unused)))
{
	char *filename = NULL;
	int ret = 0;
	int filenamelen = -1;

	if (name_type == FILE_NAME_DOS)
		return 0;
	
	if ((filenamelen = ntfs_ucstombs(name, name_len, &filename, 0)) < 0) {
		ntfs_log_perror("Filename decoding failed (inode %llu)",
				(unsigned long long)MREF(mref));
		return -1;
	}
	
	if (ntfs_fuse_is_named_data_stream(filename)) {
		ntfs_log_error("Unable to access '%s' (inode %llu) with "
				"current named streams access interface.\n",
				filename, (unsigned long long)MREF(mref));
		free(filename);
		return 0;
	} else {
		struct stat st = { .st_ino = MREF(mref) };
		 
		switch (dt_type) {
		case NTFS_DT_DIR :
			st.st_mode = S_IFDIR | (0777 & ~ctx->dmask); 
			break;
		case NTFS_DT_LNK :
			st.st_mode = S_IFLNK | 0777;
			break;
		case NTFS_DT_FIFO :
			st.st_mode = S_IFIFO;
			break;
		case NTFS_DT_SOCK :
			st.st_mode = S_IFSOCK;
			break;
		case NTFS_DT_BLK :
			st.st_mode = S_IFBLK;
			break;
		case NTFS_DT_CHR :
			st.st_mode = S_IFCHR;
			break;
		case NTFS_DT_REPARSE :
			st.st_mode = S_IFLNK | 0777; /* default */
			break;
		default : /* unexpected types shown as plain files */
		case NTFS_DT_REG :
			st.st_mode = S_IFREG | (0777 & ~ctx->fmask);
			break;
		}
		
		ret = fill_ctx->filler(fill_ctx->buf, filename, &st, 0);
	}
	
	free(filename);
	return ret;
}

int ntfs_fuse_readdir(const char *path, void *buf,
		fuse_fill_dir_t filler, off_t offset __attribute__((unused)))
{
	ntfs_fuse_fill_context_t fill_ctx;
	ntfs_inode *ni;
	s64 pos = 0;
	int err = 0;

	fill_ctx.filler = filler;
	fill_ctx.buf = buf;
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni)
		return -errno;

	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	err = -EOPNOTSUPP;
	//} else {
		if (ntfs_readdir(ni, &pos, &fill_ctx,
				(ntfs_filldir_t)ntfs_fuse_filler))
			err = -errno;
	//}
	ntfs_fuse_update_times(ni, NTFS_UPDATE_ATIME);
	if (ntfs_inode_close(ni))
		set_fuse_error(&err);
	return err;
}

int ntfs_fuse_open(const char *org_path)
{
	ntfs_inode *ni;
	ntfs_attr *na = NULL;
	int res = 0;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (ni) {
		//if (!(ni->flags & FILE_ATTR_REPARSE_POINT)) {
			na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
			if (!na) {
				res = -errno;
				goto close;
			}
		//}
		//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		//	res = -EOPNOTSUPP;
		//	goto close;
		//}
		ntfs_attr_close(na);
close:
		if (ntfs_inode_close(ni))
			set_fuse_error(&res);
	} else
		res = -errno;
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

int ntfs_fuse_read(const char *org_path, char *buf, size_t size, off_t offset)
{
	ntfs_inode *ni = NULL;
	ntfs_attr *na = NULL;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len, res;
	s64 total = 0;
	s64 max_read;

	if (!size)
		return 0;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	res = -EOPNOTSUPP;
	//	goto exit;
	//}
	na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
	if (!na) {
		res = -errno;
		goto exit;
	}
	max_read = na->data_size;
	if (offset + (off_t)size > max_read) {
		if (max_read < offset)
			goto ok;
		size = max_read - offset;
	}
	while (size > 0) {
		s64 ret = ntfs_attr_pread(na, offset, size, buf + total);
		if (ret != (s64)size)
			ntfs_log_perror("ntfs_attr_pread error reading '%s' at "
				"offset %lld: %lld <> %lld", org_path, 
				(long long)offset, (long long)size, (long long)ret);
		if (ret <= 0 || ret > (s64)size) {
			res = (ret < 0) ? -errno : -EIO;
			goto exit;
		}
		size -= ret;
		offset += ret;
		total += ret;
	}
ok:
	res = total;
	ntfs_fuse_update_times(ni, NTFS_UPDATE_ATIME);
exit:
	if (na)
		ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

int ntfs_fuse_write(const char *org_path, const char *buf, size_t size, off_t offset)
{
	ntfs_inode *ni = NULL;
	ntfs_attr *na = NULL;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len, res, total = 0;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0) {
		res = stream_name_len;
		goto out;
	}
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	res = -EOPNOTSUPP;
	//	goto exit;
	//}
	na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
	if (!na) {
		res = -errno;
		goto exit;
	}
	while (size) {
		s64 ret = ntfs_attr_pwrite(na, offset, size, buf + total);
		if (ret <= 0) {
			res = -errno;
			goto exit;
		}
		size   -= ret;
		offset += ret;
		total  += ret;
	}
	res = total;
	if (res > 0)
		ntfs_fuse_update_times(ni, NTFS_UPDATE_MCTIME);
exit:
	if (na)
		ntfs_attr_close(na);
	if (res > 0)
		set_archive(ni);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(path);
	if (stream_name_len)
		free(stream_name);
out:	
	return res;
}

/*
 *	Common part for truncate() and ftruncate()
 */

static int ntfs_fuse_trunc(const char *org_path, off_t size,
			BOOL chkwrite __attribute__((unused)))
{
	ntfs_inode *ni = NULL;
	ntfs_attr *na = NULL;
	int res;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len;
	s64 oldsize;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni)
		goto exit;
	/* deny truncating metadata files */
	if (ni->mft_no < FILE_first_user) {
		errno = EPERM;
		goto exit;
	}

	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	res = -EOPNOTSUPP;
	//	goto exit;
	//}
	na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
	if (!na)
		goto exit;
		/*
		 * For compressed files, upsizing is done by inserting a final
		 * zero, which is optimized as creating a hole when possible. 
		 */
	oldsize = na->data_size;
	if ((na->data_flags & ATTR_COMPRESSION_MASK)
	    && (size > na->initialized_size)) {
		char zero = 0;
		if (ntfs_attr_pwrite(na, size - 1, 1, &zero) <= 0)
			goto exit;
	} else
		if (ntfs_attr_truncate(na, size))
			goto exit;
	if (oldsize != size)
		set_archive(ni);

	ntfs_fuse_update_times(ni, NTFS_UPDATE_MCTIME);
	errno = 0;
exit:
	res = -errno;
	ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

int ntfs_fuse_truncate(const char *org_path, off_t size)
{
	return ntfs_fuse_trunc(org_path, size, TRUE);
}

int ntfs_fuse_ftruncate(const char *org_path, off_t size)
{
	/*
	 * in ->ftruncate() the file handle is guaranteed
	 * to have been opened for write.
	 */
	return (ntfs_fuse_trunc(org_path, size, FALSE));
}

int ntfs_fuse_chmod(const char *path, mode_t mode)
{
	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL; /* n/a for named data streams. */

	return -EOPNOTSUPP;
}

int ntfs_fuse_chown(const char *path, uid_t uid, gid_t gid)
{
	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL; /* n/a for named data streams. */

	if (uid == ctx->uid && gid == ctx->gid)
		return 0;
	return -EOPNOTSUPP;
}

int ntfs_fuse_create(const char *org_path, mode_t typemode, dev_t dev,
		const char *target)
{
	char *name;
	ntfschar *uname = NULL, *utarget = NULL;
	ntfs_inode *dir_ni = NULL, *ni;
	char *dir_path;
	le32 securid;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len;
	mode_t type = typemode & ~07777;
	mode_t perm;
	int res = 0, uname_len, utarget_len;

	dir_path = strdup(org_path);
	if (!dir_path)
		return -errno;
	/* Generate unicode filename. */
	name = strrchr(dir_path, '/');
	name++;
	uname_len = ntfs_mbstoucs(name, &uname);
	if ((uname_len < 0)
	    || (ctx->windows_names
		&& ntfs_forbidden_names(ctx->vol,uname,uname_len,TRUE))) {
		res = -errno;
		goto exit;
	}
	stream_name_len = ntfs_fuse_parse_path(org_path,
					 &path, &stream_name);
		/* stream name validity has been checked previously */
	if (stream_name_len < 0) {
		res = stream_name_len;
		goto exit;
	}
	/* Open parent directory. */
	*--name = 0;
	dir_ni = ntfs_pathname_to_inode(ctx->vol, NULL, dir_path);
		/* Deny creating files in $Extend */
	if (!dir_ni || (dir_ni->mft_no == FILE_Extend)) {
		free(path);
		res = -errno;
		if (dir_ni)
			res = -EPERM;
		goto exit;
	}
		if (S_ISDIR(type))
			perm = (typemode & ~ctx->dmask & 0777)
				| S_ISGID;
		else
			perm = typemode & ~ctx->fmask & 0777;

		securid = const_cpu_to_le32(0);
		/* Create object specified in @type. */
		//if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
		//	errno = EOPNOTSUPP;
		//} else {
			switch (type) {
				case S_IFCHR:
				case S_IFBLK:
					ni = ntfs_create_device(dir_ni, securid,
						uname, uname_len, type,	dev);
					break;
				case S_IFLNK:
					utarget_len = ntfs_mbstoucs(target,
							&utarget);
					if (utarget_len < 0) {
						res = -errno;
						goto exit;
					}
					ni = ntfs_create_symlink(dir_ni,
						securid, uname, uname_len,
						utarget, utarget_len);
					break;
				default:
					ni = ntfs_create(dir_ni, securid,
						uname, uname_len, type);
					break;
			}
		//}
		if (ni) {
			set_archive(ni);
			NInoSetDirty(ni);
			/*
			 * closing ni requires access to dir_ni to
			 * synchronize the index, avoid double opening.
			 */
			if (ntfs_inode_close_in_dir(ni, dir_ni))
				set_fuse_error(&res);
			ntfs_fuse_update_times(dir_ni, NTFS_UPDATE_MCTIME);
		} else
			res = -errno;
	free(path);

exit:
	free(uname);
	if (ntfs_inode_close(dir_ni))
		set_fuse_error(&res);
	if (utarget)
		free(utarget);
	free(dir_path);
	return res;
}

int ntfs_fuse_create_stream(const char *path,
		ntfschar *stream_name, const int stream_name_len)
{
	ntfs_inode *ni;
	int res = 0;

	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		if (res == -ENOENT) {
			/*
			 * If such file does not exist, create it and try once
			 * again to add stream to it.
			 * Note : no fuse_file_info for creation of main file
			 */
			res = ntfs_fuse_create(path, S_IFREG, 0, NULL);
			if (!res)
				return ntfs_fuse_create_stream(path,
						stream_name, stream_name_len);
			else
				res = -errno;
		}
		return res;
	}
	if (ntfs_attr_add(ni, AT_DATA, stream_name, stream_name_len, NULL, 0))
		res = -errno;
	else
		set_archive(ni);

	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	return res;
}

static int ntfs_fuse_mknod_common(const char *org_path, mode_t mode, dev_t dev)
{
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len;
	int res = 0;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	if (stream_name_len
	    && (!S_ISREG(mode)
		|| (ctx->windows_names
		    && ntfs_forbidden_names(ctx->vol,stream_name,
					stream_name_len, TRUE)))) {
		res = -EINVAL;
		goto exit;
	}
	if (!stream_name_len)
		res = ntfs_fuse_create(path, mode & (S_IFMT | 07777), dev, 
					NULL);
	else
		res = ntfs_fuse_create_stream(path, stream_name,
				stream_name_len);
exit:
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

int ntfs_fuse_mknod(const char *path, mode_t mode, dev_t dev)
{
	return ntfs_fuse_mknod_common(path, mode, dev);
}

int ntfs_fuse_create_file(const char *path, mode_t mode)
{
	return ntfs_fuse_mknod_common(path, mode, 0);
}

int ntfs_fuse_symlink(const char *to, const char *from)
{
	if (ntfs_fuse_is_named_data_stream(from))
		return -EINVAL; /* n/a for named data streams. */
	return ntfs_fuse_create(from, S_IFLNK, 0, to);
}

int ntfs_fuse_link(const char *old_path, const char *new_path)
{
	char *name;
	ntfschar *uname = NULL;
	ntfs_inode *dir_ni = NULL, *ni;
	char *path;
	int res = 0, uname_len;

	if (ntfs_fuse_is_named_data_stream(old_path))
		return -EINVAL; /* n/a for named data streams. */
	if (ntfs_fuse_is_named_data_stream(new_path))
		return -EINVAL; /* n/a for named data streams. */
	path = strdup(new_path);
	if (!path)
		return -errno;
	/* Open file for which create hard link. */
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, old_path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
	
	/* Generate unicode filename. */
	name = strrchr(path, '/');
	name++;
	uname_len = ntfs_mbstoucs(name, &uname);
	if ((uname_len < 0)
	    || (ctx->windows_names
		&& ntfs_forbidden_names(ctx->vol,uname,uname_len,TRUE))) {
		res = -errno;
		goto exit;
	}
	/* Open parent directory. */
	*--name = 0;
	dir_ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!dir_ni) {
		res = -errno;
		goto exit;
	}

	{
		//if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
		//	errno = EOPNOTSUPP;
		//	res = -errno;
		//	if (res)
		//		goto exit;
		//} else
			if (ntfs_link(ni, dir_ni, uname, uname_len)) {
					res = -errno;
				goto exit;
			}
	
		set_archive(ni);
		ntfs_fuse_update_times(ni, NTFS_UPDATE_CTIME);
		ntfs_fuse_update_times(dir_ni, NTFS_UPDATE_MCTIME);
	}
exit:
	/* 
	 * Must close dir_ni first otherwise ntfs_inode_sync_file_name(ni)
	 * may fail because ni may not be in parent's index on the disk yet.
	 */
	if (ntfs_inode_close(dir_ni))
		set_fuse_error(&res);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(uname);
	free(path);
	return res;
}

int ntfs_fuse_rm(const char *org_path)
{
	char *name;
	ntfschar *uname = NULL;
	ntfs_inode *dir_ni = NULL, *ni;
	char *path;
	int res = 0, uname_len;

	path = strdup(org_path);
	if (!path)
		return -errno;
	/* Open object for delete. */
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
	/* deny unlinking metadata files */
	if (ni->mft_no < FILE_first_user) {
		errno = EPERM;
		res = -errno;
		goto exit;
	}

	/* Generate unicode filename. */
	name = strrchr(path, '/');
	name++;
	uname_len = ntfs_mbstoucs(name, &uname);
	if (uname_len < 0) {
		res = -errno;
		goto exit;
	}
	/* Open parent directory. */
	*--name = 0;
	dir_ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
		/* deny unlinking metadata files from $Extend */
	if (!dir_ni || (dir_ni->mft_no == FILE_Extend)) {
		res = -errno;
		if (dir_ni)
			res = -EPERM;
		goto exit;
	}
	
		//if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
		//	res = -EOPNOTSUPP;
		//} else
			if (ntfs_delete(ctx->vol, org_path, ni, dir_ni,
					 uname, uname_len))
				res = -errno;
		/* ntfs_delete() always closes ni and dir_ni */
		ni = dir_ni = NULL;
exit:
	if (ntfs_inode_close(dir_ni))
		set_fuse_error(&res);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(uname);
	free(path);
	return res;
}

int ntfs_fuse_rm_stream(const char *path, ntfschar *stream_name,
		const int stream_name_len)
{
	ntfs_inode *ni;
	int res = 0;

	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni)
		return -errno;
	
	if (ntfs_attr_remove(ni, AT_DATA, stream_name, stream_name_len))
		res = -errno;

	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	return res;
}

int ntfs_fuse_unlink(const char *org_path)
{
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len;
	int res = 0;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	if (!stream_name_len)
		res = ntfs_fuse_rm(path);
	else {
		res = ntfs_fuse_rm_stream(path, stream_name, stream_name_len);
	}
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}

int ntfs_fuse_safe_rename(const char *old_path, 
				 const char *new_path, 
				 const char *tmp)
{
	int ret;

	ntfs_log_trace("Entering\n");
	
	ret = ntfs_fuse_link(new_path, tmp);
	if (ret)
		return ret;
	
	ret = ntfs_fuse_unlink(new_path);
	if (!ret) {
		
		ret = ntfs_fuse_link(old_path, new_path);
		if (ret)
			goto restore;
		
		ret = ntfs_fuse_unlink(old_path);
		if (ret) {
			if (ntfs_fuse_unlink(new_path))
				goto err;
			goto restore;
		}
	}
	
	goto cleanup;
restore:
	if (ntfs_fuse_link(tmp, new_path)) {
err:
		ntfs_log_perror("Rename failed. Existing file '%s' was renamed "
				"to '%s'", new_path, tmp);
	} else {
cleanup:
		/*
		 * Condition for this unlink has already been checked in
		 * "ntfs_fuse_rename_existing_dest()", so it should never
		 * fail (unless concurrent access to directories when fuse
		 * is multithreaded)
		 */
		if (ntfs_fuse_unlink(tmp) < 0)
			ntfs_log_perror("Rename failed. Existing file '%s' still present "
				"as '%s'", new_path, tmp);
	}
	return 	ret;
}

int ntfs_fuse_rename_existing_dest(const char *old_path, const char *new_path)
{
	int ret, len;
	char *tmp;
	const char *ext = ".ntfs-3g-";

	ntfs_log_trace("Entering\n");
	
	len = strlen(new_path) + strlen(ext) + 10 + 1; /* wc(str(2^32)) + \0 */
	tmp = ntfs_malloc(len);
	if (!tmp)
		return -errno;
	
	ret = snprintf(tmp, len, "%s%s%010d", new_path, ext, ++ntfs_sequence);
	if (ret != len - 1) {
		ntfs_log_error("snprintf failed: %d != %d\n", ret, len - 1);
		ret = -EOVERFLOW;
	} else {
		ret = ntfs_fuse_safe_rename(old_path, new_path, tmp);
	}
	free(tmp);
	return 	ret;
}

int ntfs_fuse_rename(const char *old_path, const char *new_path)
{
	int ret, stream_name_len;
	char *path = NULL;
	ntfschar *stream_name;
	ntfs_inode *ni;
	u64 inum;
	BOOL same;
	
	ntfs_log_debug("rename: old: '%s'  new: '%s'\n", old_path, new_path);
	
	/*
	 *  FIXME: Rename should be atomic.
	 */
	stream_name_len = ntfs_fuse_parse_path(new_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (ni) {
		ret = ntfs_check_empty_dir(ni);
		if (ret < 0) {
			ret = -errno;
			ntfs_inode_close(ni);
			goto out;
		}
		
		inum = ni->mft_no;
		if (ntfs_inode_close(ni)) {
			set_fuse_error(&ret);
			goto out;
		}

		free(path);
		path = (char*)NULL;
		if (stream_name_len)
			free(stream_name);

			/* silently ignore a rename to same inode */
		stream_name_len = ntfs_fuse_parse_path(old_path,
						&path, &stream_name);
		if (stream_name_len < 0)
			return stream_name_len;
	
		ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
		if (ni) {
			same = ni->mft_no == inum;
			if (ntfs_inode_close(ni))
				ret = -errno;
			else
				if (!same)
					ret = ntfs_fuse_rename_existing_dest(
							old_path, new_path);
		} else
			ret = -errno;
		goto out;
	}

	ret = ntfs_fuse_link(old_path, new_path);
	if (ret)
		goto out;
	
	ret = ntfs_fuse_unlink(old_path);
	if (ret)
		ntfs_fuse_unlink(new_path);
out:
	free(path);
	if (stream_name_len)
		free(stream_name);
	return ret;
}

int ntfs_fuse_mkdir(const char *path,
		mode_t mode)
{
	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL; /* n/a for named data streams. */
	return ntfs_fuse_create(path, S_IFDIR | (mode & 07777), 0, NULL);
}

int ntfs_fuse_rmdir(const char *path)
{
	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL; /* n/a for named data streams. */
	return ntfs_fuse_rm(path);
}

int ntfs_fuse_utime(const char *path, struct utimbuf *buf)
{
	ntfs_inode *ni;
	int res = 0;
	struct timespec actime;
	struct timespec modtime;

	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL; /* n/a for named data streams. */
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni)
		return -errno;
	
	if (buf) {
		actime.tv_sec = buf->actime;
		actime.tv_nsec = 0;
		modtime.tv_sec = buf->modtime;
		modtime.tv_nsec = 0;
		ni->last_access_time = timespec2ntfs(actime);
		ni->last_data_change_time = timespec2ntfs(modtime);
		ntfs_fuse_update_times(ni, NTFS_UPDATE_CTIME);
	} else
		ntfs_inode_update_times(ni, NTFS_UPDATE_AMCTIME);

	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	return res;
}

int ntfs_fuse_fsync(const char *path __attribute__((unused)),
			int type __attribute__((unused)))
{
	int ret;

		/* sync the full device */
	ret = ntfs_device_sync(ctx->vol->dev);
	if (ret)
		ret = -errno;
	return (ret);
}

int ntfs_fuse_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	LCN lcn;
	int ret = 0; 
	int cl_per_bl = ctx->vol->cluster_size / blocksize;

	if (blocksize > ctx->vol->cluster_size)
		return -EINVAL;
	
	if (ntfs_fuse_is_named_data_stream(path))
		return -EINVAL;
	
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni)
		return -errno;

	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		ret = -errno;
		goto close_inode;
	}
	
	if ((na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED))
			 || !NAttrNonResident(na)) {
		ret = -EINVAL;
		goto close_attr;
	}
	
	if (ntfs_attr_map_whole_runlist(na)) {
		ret = -errno;
		goto close_attr;
	}
	
	lcn = ntfs_rl_vcn_to_lcn(na->rl, *idx / cl_per_bl);
	*idx = (lcn > 0) ? lcn * cl_per_bl + *idx % cl_per_bl : 0;
	
close_attr:
	ntfs_attr_close(na);
close_inode:
	if (ntfs_inode_close(ni))
		set_fuse_error(&ret);
	return ret;
}

void ntfs_close(void)
{
	if (!ctx)
		return;
	
	if (!ctx->vol)
		return;
	
	if (ntfs_umount(ctx->vol, FALSE))
		ntfs_log_perror("Failed to close volume");
	
	ctx->vol = NULL;
}

int ntfs_fuse_init(void)
{
	ctx = ntfs_calloc(sizeof(ntfs_fuse_context_t));
	if (!ctx)
		return -1;
	
	*ctx = (ntfs_fuse_context_t) {
		.uid     = 0,
		.gid     = 0,
		.fmask   = 0,
		.dmask   = 0,
		.streams = NF_STREAMS_INTERFACE_NONE,
		.ro      = FALSE,
		.recover = TRUE,
		.windows_names = TRUE,
		.compression = FALSE,
		.hiberfile = TRUE,
		.exclusive = FALSE
	};
	return 0;
}

int ntfs_open(const char *device, BOOL ro, BOOL exclusive)
{
	unsigned long flags = 0;
	
	ctx->ro = ro;
	ctx->exclusive = exclusive;
	if (!ctx->exclusive)
		flags |= NTFS_MNT_EXCLUSIVE;
	if (ctx->ro)
		flags |= NTFS_MNT_RDONLY;
	else
		if (!ctx->hiberfile)
			flags |= NTFS_MNT_MAY_RDONLY;
	if (ctx->recover)
		flags |= NTFS_MNT_RECOVER;
	if (ctx->hiberfile)
		flags |= NTFS_MNT_IGNORE_HIBERFILE;

	ctx->vol = ntfs_mount(device, flags);
	if (!ctx->vol) {
		ntfs_log_perror("Failed to mount '%s'", device);
		goto err_out;
	}
	//if (ctx->sync && ctx->vol->dev)
	//	NDevSetSync(ctx->vol->dev);
	if (ctx->compression)
		NVolSetCompression(ctx->vol);
	else
		NVolClearCompression(ctx->vol);
	if (ntfs_set_shown_files(ctx->vol, TRUE, TRUE, TRUE))
		goto err_out;
	
	if (ntfs_volume_get_free_space(ctx->vol)) {
		ntfs_log_perror("Failed to read NTFS $Bitmap");
		goto err_out;
	}

	ctx->vol->free_mft_records = ntfs_get_nr_free_mft_records(ctx->vol);
	if (ctx->vol->free_mft_records < 0) {
		ntfs_log_perror("Failed to calculate free MFT records");
		goto err_out;
	}

	if (ctx->hiberfile && ntfs_volume_check_hiberfile(ctx->vol, 0)) {
		if (errno != EPERM)
			goto err_out;
		if (ntfs_fuse_rm("/hiberfil.sys"))
			goto err_out;
	}
	
	errno = 0;
	goto out;
err_out:
	if (!errno)
		errno = EIO;
out :
	return ntfs_volume_error(errno);
}

// add stat support
int ntfs_fuse_stat(const char *org_path, struct stat *sbuf)
{
	ntfs_inode *ni = NULL;
	ntfs_attr *na = NULL;
	char *path = NULL;
	ntfschar *stream_name;
	int stream_name_len, res;

	stream_name_len = ntfs_fuse_parse_path(org_path, &path, &stream_name);
	if (stream_name_len < 0)
		return stream_name_len;
	ni = ntfs_pathname_to_inode(ctx->vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto exit;
	}
	//if (ni->flags & FILE_ATTR_REPARSE_POINT) {
	//	res = -EOPNOTSUPP;
	//	goto exit;
	//}
	na = ntfs_attr_open(ni, AT_DATA, stream_name, stream_name_len);
	if (!na) {
		sbuf->st_size = 0;
	} else {
		sbuf->st_size = na->data_size;
	}
	sbuf->st_ino = ni->mft_no;
	sbuf->st_uid = 0;
	sbuf->st_gid = 0;
	sbuf->st_atime = ntfs2timespec(ni->last_access_time).tv_sec;
	sbuf->st_mtime = ntfs2timespec(ni->last_data_change_time).tv_sec;
	sbuf->st_ctime = ntfs2timespec(ni->last_mft_change_time).tv_sec;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
		sbuf->st_mode = S_IFDIR | 0777;
	else
		sbuf->st_mode = S_IFREG | 0777;
	// TODO: fill st_mode
	sbuf->st_nlink = ni->mrec->link_count;
	sbuf->st_dev = 0; // TODO: fill st_dev and st_rdev
	sbuf->st_rdev = 0;
	res = 0;
exit:
	if (na)
		ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		set_fuse_error(&res);
	free(path);
	if (stream_name_len)
		free(stream_name);
	return res;
}