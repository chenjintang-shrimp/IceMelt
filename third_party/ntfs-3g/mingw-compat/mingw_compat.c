/*
 * mingw_compat.c -- implementations for the mingw-w64 shim headers.
 *
 * Kept deliberately tiny: these exist so the library compiles and links on a
 * platform that has no POSIX user/group database. Every one of them returns the
 * "not found" answer that the callers already handle, and none of them is on a
 * code path the tools built here (ntfsfix, ntfscp, ntfs-3g-cli) exercise:
 * user/group lookup only happens for the FUSE driver's --user-mapping /
 * --group-mapping options.
 */

#include "ntfs_mingw_compat.h"

#include <errno.h>
#include <stddef.h>

#include "grp.h"
#include "pwd.h"

/* No /etc/passwd on Windows: report "no such entry" per POSIX. */
struct passwd* getpwnam(const char* name) {
    (void)name;
    errno = ENOENT;
    return NULL;
}

struct passwd* getpwuid(uid_t uid) {
    (void)uid;
    errno = ENOENT;
    return NULL;
}

/* No /etc/group either. */
struct group* getgrnam(const char* name) {
    (void)name;
    errno = ENOENT;
    return NULL;
}

struct group* getgrgid(gid_t gid) {
    (void)gid;
    errno = ENOENT;
    return NULL;
}
