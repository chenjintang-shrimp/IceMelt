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
#include <ctype.h>
#include <sys/stat.h>
#include <utime.h>

#include "compat.h"
#include "attrib.h"
#include "inode.h"
#include "volume.h"
#include "dir.h"
#include "unistr.h"
#include "layout.h"
#include "logfile.h"
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

/* ---- 直接挂载的探针/写入（不经过 FUSE 层）---------------------------------
 *
 * 见 ntfs-3g-fuse.h 的说明：只读探针用 NTFS_MNT_RDONLY，不重放日志、不清 dirty 标记，
 * 一个字节都不写；这正是"写之前先确认目标是什么"该有的性质。 */

/* libntfs-3g 默认是**区分大小写**的（volume.c:531 "Default with no locase table and case
 * sensitive file names"），唯一解除它的是 ntfs_set_ignore_case()，而全项目只有 lowntfs-3g
 * （FUSE 低层驱动）会调用它。ntfsfix / ntfscp / 我们这些直接挂载因此都是**区分大小写**的 ——
 * 与 Windows（NTFS 大小写不敏感）不一致。
 *
 * 这个不一致是要命的：ntfscp 在目标查不到时会 ntfs_new_file **新建一个文件**。卷上真实名字
 * 是 `system` 而传 `SYSTEM` 时，它就会在同一个目录里造出一个只有大小写不同的重名文件，真正的
 * hive 一个字节都没改，而它报成功。
 *
 * 所以我们的直接挂载一律先打开 ignore_case，行为向 Windows 看齐。 */
static void ntfs_direct_enable_ignore_case(ntfs_volume *vol)
{
	if (vol && !ntfs_set_ignore_case(vol))
		return;	/* 成功：lookup 按 $UpCase 表做大小写不敏感匹配 */
	/* 失败（locase 表建不出来）时保持区分大小写，但至少不是静默的 */
	ntfs_log_error("could not enable ignore_case; file name lookups stay case-sensitive\n");
}

int ntfs_read_file_direct(const char *device, const char *path, char *buf, size_t size,
			  long long *total_size)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	ntfs_attr *na;
	s64 want, got;
	int res;

	if (!device || !path || !buf || !size)
		return -EINVAL;
	if (total_size)
		*total_size = -1;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (!ni) {
		res = -errno;
		goto out;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		res = -errno;
		ntfs_inode_close(ni);
		goto out;
	}

	want = (s64)size;
	if (na->data_size < want)
		want = na->data_size;
	if (total_size)
		*total_size = (long long)na->data_size;
	got = ntfs_attr_pread(na, 0, want, buf);
	if (got < 0)
		res = -errno;
	else if (got != want)
		res = -EIO;
	else
		res = (int)got;

	ntfs_attr_close(na);
	ntfs_inode_close(ni);
out:
	ntfs_umount(vol, FALSE);
	return res;
}

int ntfs_raw_read_direct(const char *device, long long offset, char *buf, size_t size)
{
	ntfs_volume *vol;
	s64 got;
	int res;

	if (!device || !buf || !size || offset < 0)
		return -EINVAL;

	/* 只读直接挂载：不重放日志、不碰写路径，只借用被证明可用的 handle: 开设备
	 * 通道；随后全部走 ntfs_pread（分区相对偏移 → 驱动 → 扇区），不解释任何
	 * NTFS 结构，因此读到的就是磁盘上**本来就在那里**的字节。 */
	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;

	got = ntfs_pread(vol->dev, (s64)offset, (s64)size, buf);
	if (got < 0)
		res = -errno;
	else
		res = (int)got;

	if (ntfs_umount(vol, FALSE) && res >= 0)
		res = -EIO;
	return res;
}

int ntfs_stat_paths_direct(const char *device, const char *const *paths, int count,
			   long long *sizes)
{
	ntfs_volume *vol;
	int i, found = 0;

	if (!device || !paths || count <= 0 || !sizes)
		return -EINVAL;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	for (i = 0; i < count; ++i) {
		ntfs_inode *ni;
		ntfs_attr *na;
		sizes[i] = -1;
		if (!paths[i])
			continue;
		ni = ntfs_pathname_to_inode(vol, NULL, paths[i]);
		if (!ni)
			continue;
		na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (na) {
			sizes[i] = (long long)na->data_size;
			ntfs_attr_close(na);
		} else if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
			sizes[i] = -2;	/* 存在，是个目录 */
			++found;
			ntfs_inode_close(ni);
			continue;
		}
		ntfs_inode_close(ni);
		if (sizes[i] >= 0)
			++found;
	}
	ntfs_umount(vol, FALSE);
	return found;
}

int ntfs_set_volume_dirty_direct(const char *device, unsigned short *flags_before,
				 unsigned short *flags_after)
{
	ntfs_volume *vol;
	VOLUME_FLAGS flags;
	int res;

	if (!device)
		return -EINVAL;

	/* 要写 $Volume，所以不能只读挂载；RECOVER 与 ntfscp -f / ntfsfix 一致 —— 它们在
	 * 这个卷上都能正常挂载。 */
	vol = ntfs_mount(device, NTFS_MNT_RECOVER);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	if (flags_before)
		*flags_before = le16_to_cpu(vol->flags);
	flags = vol->flags | VOLUME_IS_DIRTY;
	res = ntfs_volume_write_flags(vol, flags);
	if (!res && flags_after)
		*flags_after = le16_to_cpu(vol->flags);
	else if (res)
		res = -errno;

	ntfs_umount(vol, FALSE);
	return res;
}

/* logstate: 的实现 —— 只读探测 $LogFile 的重启页版本与卷 dirty 位。
 *
 * 为什么需要：RW 挂载（ntfscp -f / setdirty）在重启页为 v2.0 时被 libntfs-3g
 * 无条件拒绝（volume.c 的 ntfs_volume_check_logfile → EPERM，连 RECOVER 都
 * 绕不过）：v2.0 意味着"Windows 持有该卷的缓存元数据"（fast startup / 休眠 /
 * 掉电后的状态），此时绕过文件系统写盘等于和 OS 缓存赛跑。把这个状态在写回
 * 之前显式报出来，操作者先做一次真关机（shutdown /s）或 powercfg /h off，
 * 而不是让 ntfscp 在 preflight 里摔死。只读挂载，一个字节都不写。 */
int ntfs_logstate_direct(const char *device, char *out, size_t cap)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	ntfs_attr *na;
	s64 pos, size;
	u8 *kaddr = NULL;
	RESTART_PAGE_HEADER *rph = NULL;
	size_t used = 0;
	int res = 0;

	if (!device || !out || cap < 2)
		return -EINVAL;
	out[0] = 0;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

#define LS(...) do { \
		int _n = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
		if (_n > 0) used += (size_t)_n; \
		if (used >= cap) { used = cap - 1; out[used] = 0; goto done; } \
	} while (0)

	LS("volume_flags=0x%04x dirty=%s\n",
		(unsigned)le16_to_cpu(vol->flags),
		(vol->flags & VOLUME_IS_DIRTY) ? "yes" : "no");

	ni = ntfs_inode_open(vol, FILE_LogFile);
	if (!ni) {
		LS("$LogFile: cannot open (inode %d): %s\n", FILE_LogFile, strerror(errno));
		res = 0;  /* 状态不明确但不是拒绝：留给 ntfscp 自己去撞 */
		goto done;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		LS("$LogFile: cannot open $DATA\n");
		ntfs_inode_close(ni);
		goto done;
	}
	kaddr = ntfs_malloc(NTFS_BLOCK_SIZE);
	if (!kaddr) {
		LS("$LogFile: out of memory\n");
		ntfs_attr_close(na);
		ntfs_inode_close(ni);
		goto done;
	}
	/* 与 ntfs_check_logfile 同法：只在可能是重启页起点的位置上找。
	 * $LogFile 被清空（全 0xFF）时找不到任何 RSTR/CHKD → empty。 */
	size = na->data_size;
	rph = NULL;
	for (pos = 0; pos < size && pos < (s64)MaxLogFileSize; pos = pos ? pos << 1 : (NTFS_BLOCK_SIZE >> 1)) {
		if (ntfs_attr_pread(na, pos, NTFS_BLOCK_SIZE, kaddr) != NTFS_BLOCK_SIZE)
			break;
		if (ntfs_is_empty_recordp((le32*)kaddr))
			continue;
		if (ntfs_is_rcrd_recordp((le32*)kaddr))
			break;
		if (ntfs_is_rstr_recordp((le32*)kaddr) || ntfs_is_chkd_recordp((le32*)kaddr)) {
			rph = (RESTART_PAGE_HEADER*)kaddr;
			break;
		}
	}
	if (!rph) {
		LS("logfile: EMPTY (no restart page -- emptied by ntfsfix)\n");
	} else {
		const int major = (int)sle16_to_cpu(rph->major_ver);
		const int minor = (int)sle16_to_cpu(rph->minor_ver);
		LS("logfile: magic=%c%c%c%c major=%d minor=%d system_page=%u\n",
			((const char*)&rph->magic)[0], ((const char*)&rph->magic)[1],
			((const char*)&rph->magic)[2], ((const char*)&rph->magic)[3],
			major, minor, (unsigned)le32_to_cpu(rph->system_page_size));
		if (major == 2 && minor == 0)
			LS("cached_metadata=YES -- libntfs-3g refuses RW mount of this volume\n"
			   "(fast startup / hibernation / power-cut state; do a real shutdown\n"
			   "(shutdown /s) or powercfg /h off, then retry the melt)\n");
		else
			LS("cached_metadata=no (version %d.%d is mountable)\n", major, minor);
	}
	free(kaddr);
	ntfs_attr_close(na);
	ntfs_inode_close(ni);

done:
	ntfs_umount(vol, FALSE);
	return res;
}

typedef struct {
	char *buf;
	size_t cap;
	size_t used;
	int count;
	int truncated;
} list_ctx_t;

static int list_ctx_filler(list_ctx_t *ctx, const ntfschar *name, const int name_len,
			   const int name_type, const s64 pos __attribute__((unused)),
			   const MFT_REF mref __attribute__((unused)),
			   const unsigned dt_type)
{
	char *mbs = NULL;
	int len;
	size_t need;

	if (name_type == FILE_NAME_DOS)
		return 0;
	len = ntfs_ucstombs(name, name_len, &mbs, 0);
	if (len < 0)
		return -1;

	++ctx->count;
	need = (size_t)len + 12;	/* 名字 + 制表符 + 类型 + 换行 */
	if (ctx->used + need >= ctx->cap) {
		ctx->truncated = 1;
		free(mbs);
		return 0;
	}
	ctx->buf[ctx->used++] = (dt_type == NTFS_DT_DIR) ? 'D' : 'F';
	ctx->buf[ctx->used++] = '\t';
	memcpy(ctx->buf + ctx->used, mbs, (size_t)len);
	ctx->used += (size_t)len;
	ctx->buf[ctx->used++] = '\n';
	ctx->buf[ctx->used] = 0;
	free(mbs);
	return 0;
}

int ntfs_list_dir_direct(const char *device, const char *path, char *buf, size_t cap)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	list_ctx_t ctx;
	s64 pos = 0;
	int res;

	if (!device || !path || !buf || cap < 2)
		return -EINVAL;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (!ni) {
		res = -errno;
		ntfs_umount(vol, FALSE);
		return res;
	}

	ctx.buf = buf;
	ctx.cap = cap;
	ctx.used = 0;
	ctx.count = 0;
	ctx.truncated = 0;
	buf[0] = 0;

	if (ntfs_readdir(ni, &pos, &ctx, (ntfs_filldir_t)list_ctx_filler))
		res = -errno;
	else
		res = ctx.count;

	ntfs_inode_close(ni);
	ntfs_umount(vol, FALSE);
	return res;
}

int ntfs_delete_direct(const char *device, const char *path)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	ntfs_inode *dir_ni;
	ntfschar *uname = NULL;
	const char *base;
	int name_len;
	int res;

	if (!device || !path)
		return -EINVAL;

	vol = ntfs_mount(device, NTFS_MNT_RECOVER);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (!ni) {
		res = -errno;
		ntfs_umount(vol, FALSE);
		return res;
	}

	/* ntfs_delete() 需要父目录 inode 与文件名的 Unicode 形式。注意它**总是**关闭
	 * ni 与 dir_ni（见 lowntfs-3g 的用法），所以成功路径上不能再关一次。 */
	dir_ni = ntfs_dir_parent_inode(ni);
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	name_len = ntfs_mbstoucs(base, &uname);
	if (!dir_ni || (name_len < 0)) {
		if (dir_ni)
			ntfs_inode_close(dir_ni);
		ntfs_inode_close(ni);
		ntfs_umount(vol, FALSE);
		return -EINVAL;
	}

	res = ntfs_delete(vol, (char*)NULL, ni, dir_ni, uname, (u8)name_len);
	if (res)
		res = -errno;
	free(uname);

	ntfs_umount(vol, FALSE);
	return res;
}

/* 把目录里的名字收进一个固定数组（大小写不敏感回退用） */
#define CI_MAX_NAMES 256
#define CI_MAX_NAME 96
typedef struct {
	char names[CI_MAX_NAMES][CI_MAX_NAME];
	int count;
	int overflow;
} ci_dir_t;

static int ci_dir_filler(ci_dir_t *ctx, const ntfschar *name, const int name_len,
			 const int name_type, const s64 pos __attribute__((unused)),
			 const MFT_REF mref __attribute__((unused)),
			 const unsigned dt_type __attribute__((unused)))
{
	char *mbs = NULL;
	int len;

	if (name_type == FILE_NAME_DOS)
		return 0;
	len = ntfs_ucstombs(name, name_len, &mbs, 0);
	if (len < 0)
		return -1;
	if (ctx->count < CI_MAX_NAMES && len < CI_MAX_NAME) {
		memcpy(ctx->names[ctx->count], mbs, (size_t)len + 1);
		++ctx->count;
	} else {
		ctx->overflow = 1;
	}
	free(mbs);
	return 0;
}

static int ascii_ci_equal(const char *a, const char *b)
{
	for (;; ++a, ++b) {
		unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
		if (ca != cb) return 0;
		if (!ca) return 1;
	}
}

int ntfs_attr_info_direct(const char *device, const char *path, char *out, size_t cap)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	ntfs_attr *na;
	int res = 0;
	size_t used = 0;
	int i;
	s64 allocated = 0;
	int holes = 0;

	if (!device || !path || !out || cap < 2)
		return -EINVAL;
	out[0] = 0;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (!ni) {
		res = -errno;
		ntfs_umount(vol, FALSE);
		return res;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		res = -errno;
		ntfs_inode_close(ni);
		ntfs_umount(vol, FALSE);
		return res;
	}

	/* 关键三元组：data_size 是文件长度，initialized_size 是"已初始化"的长度 ——
	 * **读到其后返回零**。两者不等就是"尾部读回零"的直接原因。 */
	used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
		"data_size=%lld initialized_size=%lld compressed_size=%lld\n",
		(long long)na->data_size, (long long)na->initialized_size,
		(long long)na->compressed_size);
	used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
		"resident=%d flags(sparse=%d compressed=%d encrypted=%d)\n",
		!NAttrNonResident(na), !!NAttrSparse(na), !!NAttrCompressed(na),
		!!NAttrEncrypted(na));

	if (ntfs_attr_map_whole_runlist(na)) {
		used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
			"runlist: FAILED to map (%s)\n", strerror(errno));
	} else {
		/* 逐段列出：lcn == -1 就是洞（**读那里返回零**）。 */
		for (i = 0; na->rl[i].length > 0; ++i) {
			if (na->rl[i].lcn < 0) ++holes;
			else allocated += na->rl[i].length;
		}
		used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
			"runlist: %d run(s), %d hole run(s), %lld cluster(s) allocated (cluster=%u)\n",
			i, holes, (long long)allocated, (unsigned)vol->cluster_size);
		for (i = 0; na->rl[i].length > 0 && i < 24; ++i) {
			used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
				"  vcn=%-8lld lcn=%-10lld len=%lld%s\n",
				(long long)na->rl[i].vcn, (long long)na->rl[i].lcn,
				(long long)na->rl[i].length,
				na->rl[i].lcn < 0 ? "  <-- HOLE (reads as zeros)" : "");
		}
		if (na->rl[i].length > 0)
			used += (size_t)snprintf(out + used, used < cap ? cap - used : 0, "  ...\n");
	}

	ntfs_attr_close(na);
	ntfs_inode_close(ni);
	ntfs_umount(vol, FALSE);
	return res;
}

/* record:<path> 的实现：把磁盘上那个 inode 的 **MFT 记录原始字节** 倾倒出来。
 *
 * 为什么需要：info: 只报解析后的字段。当 initialized_size 出现一个任何合法写入者都
 * 产生不了的值（例如 4701 —— 拷贝循环只会写 0 或 8192 的倍数，truncate 只写 0/
 * newsize/aligned）时，唯一能定案的是**看字节本身**：属性布局、序号、以及
 * $STANDARD_INFORMATION 的四个时间戳 —— 那是"最后写这条记录的是谁"的笔迹
 * （ntfscp 关路径 vs Windows 内核/还原软件，时间戳与字段次序各不相同）。
 * 只读直接挂载，一个字节都不写。 */
static const char *attr_type_name(u32 type)
{
	switch (type) {
	case AT_STANDARD_INFORMATION:	return "$STANDARD_INFORMATION";
	case AT_ATTRIBUTE_LIST:		return "$ATTRIBUTE_LIST";
	case AT_FILE_NAME:		return "$FILE_NAME";
	case AT_OBJECT_ID:		return "$OBJECT_ID";
	case AT_SECURITY_DESCRIPTOR:	return "$SECURITY_DESCRIPTOR";
	case AT_VOLUME_NAME:		return "$VOLUME_NAME";
	case AT_VOLUME_INFORMATION:	return "$VOLUME_INFORMATION";
	case AT_DATA:			return "$DATA";
	case AT_INDEX_ROOT:		return "$INDEX_ROOT";
	case AT_INDEX_ALLOCATION:	return "$INDEX_ALLOCATION";
	case AT_BITMAP:			return "$BITMAP";
	case AT_REPARSE_POINT:		return "$REPARSE_POINT";
	case AT_EA_INFORMATION:		return "$EA_INFORMATION";
	case AT_EA:			return "$EA";
	case AT_LOGGED_UTILITY_STREAM:	return "$LOGGED_UTILITY_STREAM";
	default:			return "?";
	}
}

int ntfs_record_dump_direct(const char *device, const char *path, char *out, size_t cap)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	MFT_RECORD *mrec;
	ATTR_RECORD *attr;
	size_t used = 0;
	int res = 0;

	if (!device || !path || !out || cap < 2)
		return -EINVAL;
	out[0] = 0;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;
	ntfs_direct_enable_ignore_case(vol);

	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (!ni) {
		res = -errno;
		ntfs_umount(vol, FALSE);
		return res;
	}

#define RD(...) do { \
		int _n = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
		if (_n > 0) used += (size_t)_n; \
		if (used >= cap) { used = cap - 1; out[used] = 0; goto done; } \
	} while (0)

	/* 基记录 = inode 编号；ni->mrec 是 ntfs-3g 已解析的那条（基记录）。 */
	RD("inode=%llu mft_record_size=%u\n",
		(unsigned long long)ni->mft_no, (unsigned)vol->mft_record_size);

	mrec = ni->mrec;
	RD("record: magic=%c%c%c%c seq=%u links=%u flags=0x%04x bytes_in_use=%u "
		"bytes_allocated=%u base_record_ref=0x%llx next_attr=0x%x\n",
		isprint((unsigned char)((const u8*)&mrec->magic)[0]) ? ((const u8*)&mrec->magic)[0] : '.',
		isprint((unsigned char)((const u8*)&mrec->magic)[1]) ? ((const u8*)&mrec->magic)[1] : '.',
		isprint((unsigned char)((const u8*)&mrec->magic)[2]) ? ((const u8*)&mrec->magic)[2] : '.',
		isprint((unsigned char)((const u8*)&mrec->magic)[3]) ? ((const u8*)&mrec->magic)[3] : '.',
		le16_to_cpu(mrec->sequence_number), le16_to_cpu(mrec->link_count),
		le16_to_cpu(mrec->flags), le32_to_cpu(mrec->bytes_in_use),
		le32_to_cpu(mrec->bytes_allocated),
		(unsigned long long)le64_to_cpu(mrec->base_mft_record),
		le16_to_cpu(mrec->next_attr_instance));

	/* 逐属性倾倒布局与原始长度字段 —— data_size(+0x30)/initialized_size(+0x38)
	 * 是非驻留 $DATA 的关键三元组；dump 原始字节能看出"哪半截是新的"。 */
	{
		ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
		if (!ctx) {
			RD("attr scan: FAILED to get search ctx (%s)\n", strerror(errno));
		} else {
			int idx = 0;
			while (!ntfs_attr_lookup(AT_UNUSED, NULL, 0, 0, 0, NULL, 0, ctx)) {
				attr = ctx->attr;
				const u32 type = le32_to_cpu(attr->type);
				const u32 len = le32_to_cpu(attr->length);
				RD("attr[%d]: %s(0x%x) off=%u len=%u nonres=%u name_len=%u",
					idx, attr_type_name(type), type,
					le16_to_cpu(attr->name_offset), len,
					attr->non_resident, attr->name_length);
				if (attr->non_resident) {
					RD(" lowest_vcn=%llu highest_vcn=%llu alloc=%llu "
						"data=%llu init=%llu comp=%llu",
						(unsigned long long)le64_to_cpu(attr->lowest_vcn),
						(unsigned long long)le64_to_cpu(attr->highest_vcn),
						(unsigned long long)sle64_to_cpu(attr->allocated_size),
						(unsigned long long)sle64_to_cpu(attr->data_size),
						(unsigned long long)sle64_to_cpu(attr->initialized_size),
						(attr->compression_unit
						 ? (unsigned long long)sle64_to_cpu(attr->compressed_size) : 0ull));
				} else {
					RD(" value_len=%u value_off=%u",
						le32_to_cpu(attr->value_length),
						le16_to_cpu(attr->value_offset));
					/* $STANDARD_INFORMATION 的时间戳是"最后写入者"的笔迹：
					 * ntfs-3g 关路径与 Windows/还原软件写的次序和值都不同。 */
					if (type == AT_STANDARD_INFORMATION) {
						const STANDARD_INFORMATION *si =
							(const STANDARD_INFORMATION*)((u8*)attr +
							le16_to_cpu(attr->value_offset));
						RD("\n  si: ctime=%llu atime=%llu mtime=%llu "
							"ntfs_etime=%llu",
							(unsigned long long)le64_to_cpu(si->creation_time),
							(unsigned long long)le64_to_cpu(si->last_access_time),
							(unsigned long long)le64_to_cpu(si->last_data_change_time),
							(unsigned long long)le64_to_cpu(si->last_mft_change_time));
					}
				}
				RD("\n");
				++idx;
			}
			ntfs_attr_put_search_ctx(ctx);
			if (errno != ENOENT)
				RD("attr scan stopped: %s\n", strerror(errno));
		}
	}

	/* 头 256 字节的十六进制倾倒：布局异常（错位/半新半旧）肉眼可辨。 */
	{
		const u8 *p = (const u8*)mrec;
		u32 i;
		for (i = 0; i < 256; i += 16) {
			RD("%04x  ", i);
			for (u32 j = 0; j < 16; ++j)
				RD("%02x ", p[i + j]);
			RD(" |");
			for (u32 j = 0; j < 16; ++j) {
				const u8 c = p[i + j];
				RD("%c", isprint(c) ? c : '.');
			}
			RD("|\n");
		}
	}
	RD("(seq/attr layout/raw bytes: 'who wrote this record' forensics)\n");

done:
	ntfs_inode_close(ni);
	ntfs_umount(vol, FALSE);
	return res;
}
int ntfs_resolve_path_direct(const char *device, const char *path, char *out, size_t cap)
{
	ntfs_volume *vol;
	ntfs_inode *ni;
	char *work = NULL;
	const char *parent;
	const char *base;
	const char *matched = NULL;
	int matches = 0;
	ci_dir_t *dir = NULL;
	s64 pos = 0;
	int res = 0;

	if (!device || !path || !out || cap < 2)
		return -EINVAL;
	out[0] = 0;

	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
	if (!vol)
		return -errno;

	/* 1) 先按原样解析 */
	ni = ntfs_pathname_to_inode(vol, NULL, path);
	if (ni) {
		ntfs_inode_close(ni);
		snprintf(out, cap, "%s", path);
		ntfs_umount(vol, FALSE);
		return 0;
	}

	/* 2) 查不到就拆出父目录与最后一段，列父目录做大小写不敏感匹配。
	 * 只对**最后一段**回退：若某层父目录也需要回退，路径探针会先报出来（它逐层 stat）。 */
	work = strdup(path);
	if (!work) {
		ntfs_umount(vol, FALSE);
		return -ENOMEM;
	}
	{
		char *slash = strrchr(work, '/');
		if (!slash) {
			parent = "/";
			base = work;
		} else {
			*slash = 0;
			parent = work[0] ? work : "/";
			base = slash + 1;
		}
	}

	if (!base[0]) {
		res = -ENOENT;
		goto out;
	}

	ni = ntfs_pathname_to_inode(vol, NULL, parent);
	if (!ni) {
		res = -errno;
		goto out;
	}

	dir = malloc(sizeof(ci_dir_t));
	if (!dir) {
		ntfs_inode_close(ni);
		res = -ENOMEM;
		goto out;
	}
	dir->count = 0;
	dir->overflow = 0;
	if (ntfs_readdir(ni, &pos, dir, (ntfs_filldir_t)ci_dir_filler) == 0) {
		int i;
		for (i = 0; i < dir->count; ++i) {
			if (ascii_ci_equal(dir->names[i], base)) {
				/* 第一个作为匹配结果；同时记住**有几个**大小写不敏感匹配 ——
				 * 多于一个就是"重名文件已经存在"（NTFS 按设计不允许，说明卷已经被
				 * 写坏过），此时不能再往上写。 */
				if (!matched) matched = dir->names[i];
				++matches;
			}
		}
	}
	ntfs_inode_close(ni);

	if (!matched) {
		res = -ENOENT;
		goto out;
	}

	/* 用真实名字重查一次，确认它确实能打开 */
	snprintf(out, cap, "%s%s%s", parent,
		 (parent[0] && parent[strlen(parent) - 1] == '/') ? "" : "/", matched);
	ni = ntfs_pathname_to_inode(vol, NULL, out);
	if (!ni) {
		res = -errno;
		out[0] = 0;
		goto out;
	}
	ntfs_inode_close(ni);
	/* matches >= 2 时把数量交给调用方（打印在 stderr 上，便于程序化识别） */
	if (matches > 1)
		fprintf(stderr, "resolve: AMBIGUOUS: %d case-insensitive matches\n", matches);
	res = 0;

out:
	free(dir);
	free(work);
	ntfs_umount(vol, FALSE);
	return res;
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