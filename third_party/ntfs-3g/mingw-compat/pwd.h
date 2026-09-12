/*
 * pwd.h -- minimal shim for the mingw-w64 build.
 *
 * libntfs-3g/acls.c includes <pwd.h> unconditionally and uses getpwnam() when
 * --user-mapping is given; libntfs-3g/security.c uses getpwuid() when building
 * the SID<->uid mapping table. Windows has no /etc/passwd and no uid namespace,
 * so there is nothing to look up: the implementations in mingw_compat.c return
 * NULL ("no such user"), which is what the callers already handle -- acls.c
 * logs "Invalid user" and leaves the mapping entry out.
 *
 * Only the fields the tree actually touches are declared.
 */

#ifndef NTFS_MINGW_PWD_H
#define NTFS_MINGW_PWD_H

#include <sys/types.h>

#ifndef NTFS_MINGW_UID_T
#define NTFS_MINGW_UID_T
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
};

struct passwd* getpwnam(const char* name);
struct passwd* getpwuid(uid_t uid);

#endif /* NTFS_MINGW_PWD_H */
