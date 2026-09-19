/*
 * ntfs_mingw_compat.h -- build ntfs-3g with MSYS2's mingw-w64 toolchain
 * (x86_64-w64-mingw32, msvcrt). Target: Windows 7 SP1 and later.
 *
 * Why this exists at all: the tree as shipped builds under Cygwin (MSYS2's
 * "msys" environment), which provides a full POSIX emulation layer via
 * msys-2.0.dll. That DLL requires Windows 10 / Server 2016 in current MSYS2
 * releases (Cygwin 3.5 dropped Windows 7), which makes every ntfs-3g tool
 * unusable on a Windows 7 machine -- and ntfscp is the only way IceMelt has to
 * write the exported SYSTEM hive back to the volume.
 *
 * mingw-w64 targets Windows 7 by default (_WIN32_WINNT 0x601) and links only
 * msvcrt, so the resulting binaries run on Windows 7 with no extra runtime
 * DLL. What it does not provide is a handful of POSIX APIs that Cygwin does.
 * This header (force-included into every translation unit) plus the shim
 * headers next to it fill exactly those gaps. Nothing here changes behaviour
 * on a normal POSIX system; it is only reachable from the mingw build.
 */

#ifndef NTFS_MINGW_COMPAT_H
#define NTFS_MINGW_COMPAT_H

/* ---------------------------------------------------------------------------
 * struct timespec
 *
 * include/ntfs-3g/ntfstime.h decides "does the platform already define struct
 * timespec?" like this:
 *
 *     #if !defined(st_mtime) & !defined(__timespec_defined)
 *     struct timespec { time_t tv_sec; long tv_nsec; };
 *     #endif
 *
 * `st_mtime` is a macro in Cygwin's <sys/stat.h> (so the Cygwin build skips the
 * definition), but mingw-w64 has no such macro -- while its <sys/types.h>
 * *does* define the struct, guarded by `_TIMESPEC_DEFINED`. So on mingw the
 * condition is true and we get "redefinition of 'struct timespec'" in ~18
 * translation units. `__timespec_defined` is the second hook the source offers
 * for exactly this situation; setting it makes ntfstime.h use the platform's
 * struct (identical layout: time_t tv_sec, long tv_nsec).
 */
#define __timespec_defined 1

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

/* ---------------------------------------------------------------------------
 * uid_t / gid_t
 *
 * mingw-w64 has no POSIX user/group ids, and include/ntfs-3g/security.h refers
 * to uid_t/gid_t unconditionally (struct MAPPING, struct CACHED_PERMISSIONS).
 * They are only ever compared/assigned, and the on-disk user mapping stores
 * ids as 32-bit values, so unsigned int is the honest choice.
 */
#ifndef NTFS_MINGW_UID_T
#define NTFS_MINGW_UID_T
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

/* ---------------------------------------------------------------------------
 * ffs() -- find first set bit.
 *
 * Used by libntfs-3g/logfile.c on an int (log page size), so the compiler
 * builtin is an exact, zero-cost replacement.
 */
#ifndef ffs
#define ffs(x) __builtin_ffs(x)
#endif

/* ---------------------------------------------------------------------------
 * makedev() / major() / minor()
 *
 * libntfs-3g/ea.c synthesises a dev_t from the major/minor pair stored in an
 * NTFS extended attribute (and takes them apart again). Cygwin provides these
 * via <sys/sysmacros.h> / <sys/mkdev.h>, neither of which exists on mingw.
 *
 * Layout is the glibc/Linux one, for consistency with the Cygwin build. Note
 * mingw's dev_t is 32 bits, so the `(major & ~0xfff) << 32` term is dropped by
 * the compiler -- which is equally true of Cygwin's 32-bit dev_t, so the two
 * builds encode identically. This value never reaches the volume: the on-disk
 * representation is a pair of separate 32-bit fields (device.major/minor), so
 * the packing only has to be self-consistent.
 *
 * Function-like macros on purpose: `device.major` (a struct field in ea.c) must
 * not be rewritten.
 */
#define makedev(maj, min)                                                     \
    ((dev_t)((((unsigned)(maj) & 0xfff) << 8) | ((unsigned)(min) & 0xff) |    \
             (((unsigned)(min) & ~0xffu) << 12)))
#define major(dev) ((unsigned)((unsigned)(dev) >> 8) & 0xfffu)
#define minor(dev)                                                            \
    ((unsigned)(((unsigned)(dev) & 0xffu) | (((unsigned)(dev) >> 12) & ~0xffu)))

/* ---------------------------------------------------------------------------
 * File-type and mode bits mingw's <sys/stat.h> does not define.
 *
 * S_IFMT/S_IFDIR/S_IFCHR/S_IFBLK/S_IFREG/S_IFIFO and S_ISDIR/S_ISREG/S_ISCHR/
 * S_ISBLK/S_ISFIFO *are* present; the rest of the family is not (verified by
 * compiling a probe against each name). Values are the standard POSIX ones.
 * S_ISLNK/S_ISSOCK are function-like because their base S_IF* was missing.
 */
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISUID
#define S_ISUID 0x800 /* set-user-ID on execution */
#endif
#ifndef S_ISGID
#define S_ISGID 0x400 /* set-group-ID on execution */
#endif
#ifndef S_ISVTX
#define S_ISVTX 0x200 /* sticky bit */
#endif

/* ---------------------------------------------------------------------------
 * Cygwin/Linux errno codes mingw does not have.
 *
 * Both are produced by libntfs-3g/win32_io.c when it maps a Win32 error onto
 * an errno; neither is compared anywhere in the tree (verified by grep), so the
 * only requirement is that each is distinct from the errno values mingw itself
 * uses. 56 is EBADRQC's value on Linux; 131 is free in mingw's errno table
 * (132 is EOVERFLOW there, which rules out Cygwin's ENOSHARE value).
 */
#ifndef EBADRQC
#define EBADRQC 56
#endif
#ifndef ENOSHARE
#define ENOSHARE 131
#endif

/* ---------------------------------------------------------------------------
 * random() / srandom()
 *
 * BSD-style PRNG, present on Cygwin/Linux, absent from mingw. Only one caller
 * exists -- libntfs-3g/security.c:ntfs_generate_guid(), which fills a GUID with
 * random bytes and whose own comment already says "perhaps not a very good
 * random number generator though...". It is called at most once, by mkntfs,
 * which this build does not produce. Aliasing onto the C library generator
 * therefore costs nothing that was not already weak.
 */
#ifndef random
#define random() rand()
#endif
#ifndef srandom
#define srandom(seed) srand(seed)
#endif

/* ---------------------------------------------------------------------------
 * getuid() / getgid()
 *
 * mingw has no POSIX user ids. libntfs-3g/security.c uses them in the
 * secure-API entry point: it only takes the privileged path when `!getuid()`
 * (i.e. "running as root"), and stores the ids as the default owner for the
 * mapping. Reporting 0 is the faithful analogue: IceMelt runs elevated, and 0
 * is the id the Cygwin build would report there too.
 */
#ifndef getuid
#define getuid() ((uid_t)0)
#endif
#ifndef getgid
#define getgid() ((gid_t)0)
#endif

#endif /* NTFS_MINGW_COMPAT_H */
