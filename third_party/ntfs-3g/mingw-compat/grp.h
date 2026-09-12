/*
 * grp.h -- minimal shim for the mingw-w64 build.
 *
 * libntfs-3g/acls.c includes <grp.h> unconditionally and uses getgrnam() for
 * --group-mapping; libntfs-3g/security.c uses getgrgid() when linking a user to
 * its groups. Same story as pwd.h: Windows has no group database, so the
 * implementations in mingw_compat.c return NULL and the callers treat that as
 * "unknown group".
 */

#ifndef NTFS_MINGW_GRP_H
#define NTFS_MINGW_GRP_H

#include <sys/types.h>

#ifndef NTFS_MINGW_UID_T
#define NTFS_MINGW_UID_T
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

struct group {
    char* gr_name;
    char* gr_passwd;
    gid_t gr_gid;
    char** gr_mem;
};

struct group* getgrnam(const char* name);
struct group* getgrgid(gid_t gid);

#endif /* NTFS_MINGW_GRP_H */
