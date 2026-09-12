/*
 * syslog.h -- minimal shim for the mingw-w64 build.
 *
 * libntfs-3g/ioctl.c includes <syslog.h> but never calls anything from it (the
 * include is the only occurrence of the name in that file); acls.c guards its
 * own include with HAVE_SYSLOG_H. mingw-w64 has no syslog at all, so this stub
 * exists purely to let those includes resolve. Nothing logs through it.
 */

#ifndef NTFS_MINGW_SYSLOG_H
#define NTFS_MINGW_SYSLOG_H

#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_NDELAY 0x08

/* Unused by the tools built here; declared so the header is self-consistent. */
void openlog(const char* ident, int option, int facility);
void closelog(void);
void syslog(int priority, const char* format, ...);

#endif /* NTFS_MINGW_SYSLOG_H */
