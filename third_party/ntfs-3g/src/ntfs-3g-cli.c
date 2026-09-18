#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <wchar.h>
#include "ntfs-3g-fuse.h"
#include "ntfs-3g-cli.h"
/* ntfs_malloc/ntfs_free 声明在 misc.h。 */
#include "misc.h"
/* ntfs_ucstombs/ntfs_mbstoucs 本应来自 unistr.h，但那个头会带入 layout.h，
   而 layout.h 无条件定义 GUID / SID_IDENTIFIER_AUTHORITY，与本文件已包含的
   <windows.h> 冲突。这里只取这两个原型（types.h 自带 <windef.h> 适配）。 */
#include "types.h"
extern int ntfs_ucstombs(const ntfschar *ins, const int ins_len, char **outs,
                         int outs_len);
extern int ntfs_mbstoucs(const char *ins, ntfschar **outs);

const char *EXEC_NAME = "ntfs-3g";

#define RW_BUFFER_SIZE 1048576

char** parse_cmd(char* CmdLine, int* _argc) {
	char** argv;
	char* _argv;
	ULONG len, argc;
	ULONG i, j;
	char a;
	BOOL in_QM, in_TEXT, in_SPACE;

	len = strlen(CmdLine);
	i = ((len + 2) / 2) * sizeof(PVOID) + sizeof(PVOID);

	argv = (char**)GlobalAlloc(GMEM_FIXED, i + (len + 2) * sizeof(char));

	_argv = (char*)(((PUCHAR)argv) + i);

	argc = 0;
	argv[argc] = _argv;
	in_QM = FALSE;
	in_TEXT = FALSE;
	in_SPACE = TRUE;
	i = 0;
	j = 0;

	while (a = CmdLine[i]) {
		if (in_QM) {
			if (a == '\"') {
				in_QM = FALSE;
			}
			else {
				_argv[j] = a;
				j++;
			}
		}
		else {
			switch (a) {
			case '\"':
				in_QM = TRUE;
				in_TEXT = TRUE;
				if (in_SPACE) {
					argv[argc] = _argv + j;
					argc++;
				}
				in_SPACE = FALSE;
				break;
			case ' ':
			case '\t':
			case '\n':
			case '\r':
				if (in_TEXT) {
					_argv[j] = '\0';
					j++;
				}
				in_TEXT = FALSE;
				in_SPACE = TRUE;
				break;
			default:
				in_TEXT = TRUE;
				if (in_SPACE) {
					argv[argc] = _argv + j;
					argc++;
				}
				_argv[j] = a;
				j++;
				in_SPACE = FALSE;
				break;
			}
		}
		i++;
	}
	_argv[j] = '\0';
	argv[argc] = NULL;

	(*_argc) = argc;
	return argv;
}

const char *help_text_simple[] = {
	"help [command]",
	"cd <path>",
	"ls [path]",
	"cp <source> <dest>",
	"mv <source> <dest>",
	"rm <path>",
	"trunc <file> <size>",
	"readlink <path>",
	"link <source> <dest>",
	"symlink <source> <dest>",
	"unlink <path>",
	"mkdir <path>",
	"fetch <file> <dest>",
	"cat <file>",
	"cpdir <source> <dest>",
	"rmdir <path>",
	"fetchdir <source> <dest>",
	"stat <file>",
	"exit"
};

const char *help_text[] = {
	"display help for a command",
	"change current directory",
	"list contents of a directory",
	"copy source file (in the outside of the filesystem) to destination",
	"move source to destination",
	"remove a file",
	"truncate a file to size",
	"read link",
	"create a link",
	"create a symbolic link",
	"delete a link",
	"create a directory",
	"fetch a file from the filesystem",
	"display the contents of the file",
	"copy source directory (in the outside of the filesystem) to destination",
	"remove a directory",
	"fetch a directory from the filesystem",
	"get information of a file",
	"unmount the filesystem and exit the program"
};

const int number_of_cmd = 19;

void print_help(const char *name) {
	int i;
	if (!strcmp(name, "help")) i = 0;
	else if (!strcmp(name, "cd")) i = 1;
	else if (!strcmp(name, "ls")) i = 2;
	else if (!strcmp(name, "cp")) i = 3;
	else if (!strcmp(name, "mv")) i = 4;
	else if (!strcmp(name, "rm")) i = 5;
	else if (!strcmp(name, "trunc")) i = 6;
	else if (!strcmp(name, "readlink")) i = 7;
	else if (!strcmp(name, "link")) i = 8;
	else if (!strcmp(name, "symlink")) i = 9;
	else if (!strcmp(name, "unlink")) i = 10;
	else if (!strcmp(name, "mkdir")) i = 11;
	else if (!strcmp(name, "fetch")) i = 12;
	else if (!strcmp(name, "cat")) i = 13;
	else if (!strcmp(name, "cpdir")) i = 14;
	else if (!strcmp(name, "rmdir")) i = 15;
	else if (!strcmp(name, "fetchdir")) i = 16;
	else if (!strcmp(name, "stat")) i = 17;
	else if (!strcmp(name, "exit")) i = 18;
	else {
		printf("unknown command '%s'\n", name);
		return;
	}
	printf("usage: %s\n\n%s\n", help_text_simple[i], help_text[i]);
}

void cmd_help(int argc, char **argv) {
	if (argc <= 1) {
		printf("available commands:\n");
		for (int i = 0; i < number_of_cmd; i++) {
			printf("    %s\n", help_text_simple[i]);
		}
		printf("\n");
		return;
	}
	print_help(argv[1]);
}

char curdir[512];
BOOL interrupt = FALSE, nocli = FALSE;
HANDLE piperead, pipewrite;

char *wstrconv(const wchar_t *wstr) {
	char *ret = NULL;
	if (ntfs_ucstombs(wstr, wcslen(wstr), &ret, 0) < 0) {
		ret = (char *)ntfs_malloc(1);
		ret[0] = '\0';
	}
	return ret;
}

wchar_t *charconv(const char *str) {
	wchar_t *ret = NULL;
	if (ntfs_mbstoucs(str, &ret) < 0) {
		ret = (wchar_t *)ntfs_malloc(2);
		ret[0] = L'\0';
	}
	return ret;
}

typedef BOOL(*LPFN_FILE_OP_FUNC)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL MyFileOP(LPFN_FILE_OP_FUNC lpOpFunc, HANDLE hFile, LPVOID lpBuffer, DWORD dwSize, LPDWORD dwReturnSize) {
	*dwReturnSize = 0;
	do {
		DWORD dwRtn = 0;
		if (!lpOpFunc(hFile, lpBuffer, dwSize, &dwRtn, NULL))
			return FALSE;
		lpBuffer = ((BYTE *)lpBuffer) + dwRtn;
		dwSize -= dwRtn;
		*dwReturnSize += dwRtn;
	} while (dwSize > 0);
	return TRUE;
}

/* 与 ntfscp 的 ntfs_utils_unix_path() 同义：只把 '\\' 换成 '/'。
 * ntfs_pathname_to_inode 只认 '/' 作分隔符，反斜杠会被当成文件名字符（踩过一次：
 * 传 "\Windows\System32\config\SYSTEM" 进去，整串被当成一个文件名 → 读到 0 字节）。 */
static char *unix_path(const char *in) {
	char *out = strdup(in);
	int i;
	if (!out) return NULL;
	for (i = 0; out[i]; i++)
		if (out[i] == '\\') out[i] = '/';
	return out;
}

char *gen_path(const char *origpath) {
	char *pth = malloc(2048);
	memset(pth, 0, sizeof(pth));
	strcpy(pth, curdir);
	if (origpath == NULL) return pth;
	if (origpath[0] == '/') strcpy(pth, origpath);
	else {
		if (curdir[strlen(curdir) - 1] != '/') sprintf(pth, "%s/%s", curdir, origpath);
		else sprintf(pth, "%s%s", curdir, origpath);
		int pthlen = strlen(pth);
		if (pth[pthlen - 1] == '/') pth[pthlen - 1] = '\0';
	}
	for (int i = 0; pth[i]; i++) {
		if (pth[i] == '/') {
			int skip_start = i + 1;
			char *nex = strchr(pth + i + 1, '/');
			int flag = 0;
			if (nex) {
				flag = 1;
				*nex = 0;
			}
			if (pth[i + 1] == 0 || !strcmp(pth + i + 1, ".")) goto skip_cur;
			else if (!strcmp(pth + i + 1, "..")) {
				skip_start = 1;
				for (int j = i - 1; j >= 0; --j) {
					if (pth[j] == '/') {
						skip_start = j + 1;
						break;
					}
				}
				goto skip_cur;
			}
			if (flag) *nex = '/';
			continue;
		skip_cur:
			if (flag) *nex = '/';
			if (nex) {
				memmove(pth + skip_start, nex + 1, strlen(nex + 1) + 1);
			} else {
				pth[skip_start - 1] = 0;
				break;
			}
			i = skip_start - 2;
		}
	}
	if (pth[0] == '\0') {
		pth[0] = '/';
		pth[1] = '\0';
	}
	return pth;
}

void cmd_cd(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	strcpy(curdir, pth);
	free(pth);
}

int list_dir_filler_getcount(void *buf, char *name, struct stat *st, int unused) {
	(*((int *)buf))++;
	return 0;
}

static int file_count = 0;

int list_dir_filler(void *buf, char *name, struct stat *st, int unused) {
	file_count++;
	if (!nocli) {
		if (S_ISDIR(st->st_mode)) printf("<DIR> ");
		else if (S_ISREG(st->st_mode)) printf("<FILE> ");
		else if (S_ISLNK(st->st_mode)) printf("<LINK> ");
		else printf("<UNKNOWN> ");
		printf("%s\n", name);
		if (interrupt) {
			interrupt = FALSE;
			printf("ERROR: User interrupted.\n");
			return 1;
		}
	} else {
		if (file_count > *((int *)buf)) return 0;
		wchar_t *tmp = charconv(name);
		wchar_t res[512];
		wcscpy(res, tmp);
		ntfs_free(tmp);
		DWORD dwWrite;
		MyFileOP((LPFN_FILE_OP_FUNC)WriteFile, pipewrite, res, sizeof(res), &dwWrite);
	}
	return 0;
}

void list_dir(const char *path) {
	file_count = 0;
	if (errno = -ntfs_fuse_readdir(path, NULL, list_dir_filler, 0)) perror("ls");
}

void cmd_ls(int argc, char **argv) {
	if (argc == 2) {
		char *pth = gen_path(argv[1]);
		list_dir(pth);
		free(pth);
	} else if (argc == 1) list_dir(curdir);
	else return print_help(argv[0]);
}

int ntfs_copy(const char *src, const char *dest) {
	FILE *fp = fopen(src, "rb");
	int err = 0;
	if (!fp) {
		err = errno;
		errno = err;
		perror("cp");
		return err;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *pth = gen_path(dest);
	if (ntfs_fuse_open(pth)) {
		if (errno = -ntfs_fuse_create_file(pth, S_IFREG | 0777)) perror("create");
	}
	if (errno = -ntfs_fuse_truncate(pth, size)) perror("truncate");
	err = errno = 0;
	int rs = -1;
	size_t off = 0;
	char buf[RW_BUFFER_SIZE];
	while ((rs = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (interrupt) {
			printf("ERROR: User interrupted.\n");
			break;
		}
		int t = ntfs_fuse_write(pth, buf, rs, off);
		if (t < 0) {
			err = errno = -t;
			perror("cp");
		}
		off += rs;
	}
	fclose(fp);
	if (rs < 0) {
		err = errno = -rs;
		perror("cp");
	}
	free(pth);
	if (interrupt) interrupt = FALSE;
	return err;
}

void cmd_cp(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	ntfs_copy(argv[1], argv[2]);
}

void cmd_mv(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	char *pth1 = gen_path(argv[1]), *pth2 = gen_path(argv[2]);
	if (errno = -ntfs_fuse_rename(pth1, pth2)) perror("mv");
	free(pth1);
	free(pth2);
}

void cmd_rm(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	if (errno = -ntfs_fuse_rm(pth)) perror("rm");
	free(pth);
}

void cmd_trunc(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	if (errno = -ntfs_fuse_truncate(pth, atol(argv[2]))) perror("trunc");
	free(pth);
}

void cmd_readlink(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	char outbuf[1024];
	if (errno = -ntfs_fuse_readlink(pth, outbuf, sizeof(outbuf))) perror("readlink");
	else printf("%s\n", outbuf);
	free(pth);
}

void cmd_link(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	char *pth1 = gen_path(argv[1]), *pth2 = gen_path(argv[2]);
	if (errno = -ntfs_fuse_link(pth1, pth2)) perror("link");
	free(pth1);
	free(pth2);
}

void cmd_symlink(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	char *pth1 = gen_path(argv[1]), *pth2 = gen_path(argv[2]);
	if (errno = -ntfs_fuse_symlink(pth1, pth2)) perror("symlink");
	free(pth1);
	free(pth2);
}

void cmd_unlink(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	if (errno = -ntfs_fuse_unlink(pth)) perror("unlink");
	free(pth);
}

void cmd_mkdir(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	if (errno = -ntfs_fuse_mkdir(pth, 0777)) perror("mkdir");
	free(pth);
}

int ntfs_fetch(const char *src, const char *dest) {
	FILE *fp = fopen(dest, "wb");
	int err = 0;
	if (!fp) {
		err = errno;
		errno = err;
		perror("fetch");
		return err;
	}
	char *pth = gen_path(src);
	int rs = -1;
	size_t off = 0;
	char buf[RW_BUFFER_SIZE];
	while ((rs = ntfs_fuse_read(pth, buf, sizeof(buf), off)) > 0) {
		if (interrupt) {
			printf("ERROR: User interrupted.\n");
			break;
		}
		int t = fwrite(buf, rs, 1, fp);
		if (t < 0) {
			err = errno = -t;
			perror("fetch");
		}
		off += rs;
	}
	fclose(fp);
	if (rs < 0) {
		err = errno = -rs;
		perror("fetch");
	}
	free(pth);
	if (interrupt) interrupt = FALSE;
	return errno = err;
}

void cmd_fetch(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	ntfs_fetch(argv[1], argv[2]);
}

void cmd_cat(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	int rs = -1;
	size_t off = 0;
	char buf[RW_BUFFER_SIZE];
	while ((rs = ntfs_fuse_read(pth, buf, sizeof(buf), off)) > 0) {
		if (interrupt) {
			printf("ERROR: User interrupted.\n");
			break;
		}
		int t = fwrite(buf, rs, 1, stdout);
		if (t < 0) {
			errno = -t;
			perror("cat");
		}
		off += rs;
	}
	if (rs < 0) {
		errno = -rs;
		perror("cat");
	}
	free(pth);
	if (interrupt) interrupt = FALSE;
}

void ntfs_cpdir(const char *src, const char *dest) {
	DIR *dir = opendir(src);
	if (dir == NULL) {
		perror("cpdir");
		return;
	}
	char *pth = gen_path(dest);
	if (errno = -ntfs_fuse_mkdir(pth, 0777)) perror("mkdir");
	char tmpdir[512];
	strcpy(tmpdir, curdir);
	strcpy(curdir, pth);
	struct dirent *ptr;
	errno = 0;
	while ((ptr = readdir(dir)) != NULL) {
		if (!strcmp(ptr->d_name, ".") || !strcmp(ptr->d_name, "..")) continue;
		char fn[512];
		sprintf(fn, "%s/%s", src, ptr->d_name);
		/*
		 * Not d_type: mingw's struct dirent has no such member (Cygwin's
		 * does), and asking the filesystem directly works on both. fn is
		 * a local path -- the side being read -- so the C library stat is
		 * the right call.
		 */
		struct stat entry_st;
		if (!stat(fn, &entry_st) && S_ISDIR(entry_st.st_mode))
			ntfs_cpdir(fn, ptr->d_name);
		else errno = ntfs_copy(fn, ptr->d_name);
	}
	closedir(dir);
	strcpy(curdir, tmpdir);
	free(pth);
}

void cmd_cpdir(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	ntfs_cpdir(argv[1], argv[2]);
}

void ntfs_rmdir(const char *dir);

int rmdir_filler(void *buf, char *name, struct stat *st, int unused) {
	if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
	if (st->st_mode & S_IFDIR) {
		ntfs_rmdir(name);
	} else {
		char *pth = gen_path(name);
		if (errno = -ntfs_fuse_rm(pth)) perror("rm");
		free(pth);
	}
	if (interrupt) {
		interrupt = FALSE;
		printf("ERROR: User interrupted.\n");
		return 1;
	}
	return 0;
}

void ntfs_rmdir(const char *dir) {
	char *pth = gen_path(dir);
	char tmpdir[512];
	strcpy(tmpdir, curdir);
	strcpy(curdir, pth);
	if (errno = -ntfs_fuse_readdir(pth, NULL, rmdir_filler, 0)) perror("rmdir");
	if (errno = -ntfs_fuse_rmdir(pth)) perror("rmdir");
	strcpy(curdir, tmpdir);
	free(pth);
}

void cmd_rmdir(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	ntfs_rmdir(argv[1]);
}

void ntfs_fetchdir(const char *src, const char *dest);

int fetchdir_filler(void *buf, char *name, struct stat *st, int unused) {
	if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
	if (st->st_mode & S_IFDIR) ntfs_fetchdir(name, name);
	else errno = ntfs_fetch(name, name);
	if (interrupt) {
		interrupt = FALSE;
		printf("ERROR: User interrupted.\n");
		return 1;
	}
	return 0;
}

void ntfs_fetchdir(const char *src, const char *dest) {
	char *pth = gen_path(src);
	char tmpdir1[512];
	strcpy(tmpdir1, curdir);
	strcpy(curdir, pth);
	char tmpdir2[MAX_PATH];
	getcwd(tmpdir2, MAX_PATH);
#ifdef __MINGW32__
	/*
	 * mingw-w64's mkdir() is the CRT's one-argument _mkdir(); the POSIX
	 * two-argument form with a mode exists only on Cygwin. The mode is
	 * meaningless on Windows anyway -- a fresh directory gets the default
	 * ACL -- so there is nothing to emulate.
	 */
	mkdir(dest);
#else
	mkdir(dest, 0777);
#endif
	chdir(dest);
	if (errno = -ntfs_fuse_readdir(pth, NULL, fetchdir_filler, 0)) perror("fetchdir");
	strcpy(curdir, tmpdir1);
	chdir(tmpdir2);
	free(pth);
}

void cmd_fetchdir(int argc, char **argv) {
	if (argc != 3) return print_help(argv[0]);
	ntfs_fetchdir(argv[1], argv[2]);
}

char *format_time(time_t t) {
	struct tm *local;
	local = localtime(&t);
	char *buf = malloc(128);
	strftime(buf, 128, "%Y-%m-%d %H:%M:%S", local);
	return buf;
}

void cmd_stat(int argc, char **argv) {
	if (argc != 2) return print_help(argv[0]);
	char *pth = gen_path(argv[1]);
	struct stat sbuf;
	memset(&sbuf, 0, sizeof(sbuf));
	if (errno = -ntfs_fuse_stat(pth, &sbuf)) perror("stat");
	printf("Type: ");
	if (sbuf.st_mode & S_IFDIR) printf("Directory\n");
	else printf("File\n");
	printf("Link count: %hd\n", sbuf.st_nlink);
	printf("Size: %lld\n", sbuf.st_size);
	printf("Access time: %s\n", format_time(sbuf.st_atime));
	printf("Modify time: %s\n", format_time(sbuf.st_mtime));
	free(pth);
}

void sighldr(int sig) {
	if (sig == SIGINT) interrupt = TRUE;
}

int main(int argc, char **argv) {
	if (argc != 2 && argc != 3 && argc != 4 && argc != 6) {
		fprintf(stderr, "usage: %s <device>[*option1*option2*...] [command | control pipe]\n", EXEC_NAME);
		fprintf(stderr, "commands: setdirty | stat:<path> | readhead <ntfsPath> <localOut> <bytes>\n");
		return 1;
	}
	signal(SIGINT, sighldr);
	/* ---- 一次性命令：自己直接挂载，不经过 FUSE 层 -----------------------------
	 *
	 * 放在 ntfs_fuse_init/ntfs_open **之前**，因为：
	 *   * ntfsfix / ntfscp 用的就是直接挂载这条路（utils_mount_volume → ntfs_mount），
	 *     它在这个 handle: 卷上被证明可用；而 FUSE 层会附加 EXCLUSIVE / IGNORE_HIBERFILE、
	 *     读 $Bitmap 与 $MFT 位图、甚至处理 hiberfil.sys —— 对"读一下这个路径装的是什么"
	 *     既没必要，还多出改动卷的机会。
	 *   * 只读探针用 NTFS_MNT_RDONLY 挂载：**不重放日志、不清 dirty 标记，一个字节都不写**。
	 *     （这正是之前用 ntfs_open 探测时最大的问题 —— 那个挂载本身可能改卷。）
	 */
	if (argc == 3 || (argc == 6 && (!strcmp(argv[2], "readhead") ||
	                                !strcmp(argv[2], "rawread")))) {
		/* setdirty：置卷的 VOLUME_IS_DIRTY 标记 —— chkdsk /f 对在用卷做的就是这件事。
		 * 复用 libntfs-3g 的 ntfs_volume_write_flags()，不手搓 $Volume 的 MFT 偏移。 */
		if (argc == 3 && !strcmp(argv[2], "setdirty")) {
			unsigned short before = 0, after = 0;
			int rs = ntfs_set_volume_dirty_direct(argv[1], &before, &after);
			if (rs < 0) {
				fprintf(stderr, "setdirty failed: %s\n", strerror(-rs));
				return 1;
			}
			printf("volume flags: 0x%04X -> 0x%04X (VOLUME_IS_DIRTY set)\n", before, after);
			return 0;
		}

		/* stat:<ntfsPath>：解析路径到 inode。给"读不到"当诊断用 ——
		 * ntfs_pathname_to_inode 逐层查找，任一层缺失都只报 ENOENT；逐层 stat 一遍就能
		 * 看出是**哪一层**断的（是 /Windows 不在，还是最末一层 SYSTEM 不在）。 */
		if (argc == 3 && !strncmp(argv[2], "stat:", 5)) {
			char *pth = unix_path(argv[2] + 5);
			const char *paths[1];
			long long size = -1;
			int found;
			paths[0] = pth;
			found = ntfs_stat_paths_direct(argv[1], paths, 1, &size);
			if (found < 0) {
				fprintf(stderr, "stat: mount failed: %s\n", strerror(-found));
				free(pth);
				return 1;
			}
			if (size == -1) {
				fprintf(stderr, "stat: %s: No such file or directory\n", pth);
				free(pth);
				return 1;
			}
			printf("stat: %s size=%lld%s\n", pth, size,
			       size == -2 ? " (directory)" : "");
			free(pth);
			return 0;
		}

		/* readhead <ntfsPath> <localOut> <bytes>：把 ntfs-3g 看到的文件开头若干字节抄到
		 * 本地文件。存在的理由：活动 SYSTEM hive 被内核独占持有（按 Win32 路径
		 * CreateFileW 会得到 ERROR_SHARING_VIOLATION=32），所以没法用普通 API 打开它
		 * 校验；而校验**必须**看 ntfs-3g 的视图 —— ntfscp 就是按这个视图写盘的。
		 *
		 * 后两个参数**两种顺序都接受**（哪个是纯数字就当字节数）：文档里写错过一次顺序，
		 * 与其让人记住，不如让它容错。 */
		if (argc == 6 && !strcmp(argv[2], "readhead")) {
			const char *outPath = NULL;
			const char *countStr = NULL;
			long long want = 0;
			long long total = -1;
			char *pth;
			char *buf;
			FILE *out;
			int rs;

			{
				char *endA = NULL, *endB = NULL;
				const long long a = strtoll(argv[4], &endA, 10);
				const long long b = strtoll(argv[5], &endB, 10);
				if (endA && *endA == 0 && argv[4][0] != 0) {
					countStr = argv[4];
					outPath = argv[5];
					want = a;
				} else if (endB && *endB == 0 && argv[5][0] != 0) {
					countStr = argv[5];
					outPath = argv[4];
					want = b;
				}
				(void)countStr;
			}
			if (want <= 0) {
				fprintf(stderr, "readhead: bad byte count (got '%s' and '%s'; need one number and "
				                "one output path)\n", argv[4], argv[5]);
				return 1;
			}
			pth = unix_path(argv[3]);
			buf = malloc((size_t)want);
			if (!buf) {
				perror("malloc");
				free(pth);
				return 1;
			}
			rs = ntfs_read_file_direct(argv[1], pth, buf, (size_t)want, &total);
			if (rs < 0) {
				/* 读不到**不等于**"内容不是 hive"：调用方要靠这个区分"读失败"与
				 * "读到了别的东西"，否则会给出错误诊断。 */
				fprintf(stderr, "readhead: %s: %s\n", pth, strerror(-rs));
				free(pth);
				free(buf);
				return 1;
			}
			out = fopen(outPath, "wb");
			if (!out) {
				perror("fopen");
				free(pth);
				free(buf);
				return 1;
			}
			fwrite(buf, 1, (size_t)rs, out);
			fclose(out);
			/* 顺带报文件总长：调用方要用它判断"我们只读了前 N 字节"还是"整份都读到了"，
			 * 以及解析 base block 时才有正确的文件长度可用（否则 root cell 校验没意义）。 */
			printf("readhead: %d of %lld bytes from %s (file size %lld)\n", rs, want, pth,
			       total);
			free(pth);
			free(buf);
			return rs == (int)want ? 0 : 1;
		}

		/* rawread <byteOffset> <localOut> <bytes>：分区内的**裸扇区读**，不经任何
		 * NTFS 语义（不会按 initialized_size/hole 合成零）。readhead/info:/record:
		 * 都只能看 NTFS 视图：一个"读回来全是零"可能是写没落盘，也可能是元数据的
		 * initialized_size 没提交而数据其实躺在簇里 —— 这两者用 NTFS 视图**永远
		 * 分不开**。rawread 直接读 info: 报的 LCN × cluster_size 那些扇区：
		 * 扇区里是期望的字节 → 数据落盘了，去查元数据提交/卸载；扇区里是零 →
		 * 数据真没写进去，去查驱动/SCSI 写路径。
		 * 偏移认十进制或 0x 十六进制；后两个参数与 readhead 一样两种顺序都接受。 */
		if (argc == 6 && !strcmp(argv[2], "rawread")) {
			const char *outPath = NULL;
			long long want = 0;
			long long off;
			char *buf;
			FILE *out;
			char *end = NULL;
			int rs;

			off = strtoll(argv[3], &end, 0);
			if (!end || *end != 0 || argv[3][0] == 0 || off < 0) {
				fprintf(stderr, "rawread: bad byte offset '%s' (want decimal or 0x hex)\n",
				        argv[3]);
				return 1;
			}
			{
				char *endA = NULL, *endB = NULL;
				const long long a = strtoll(argv[4], &endA, 0);
				const long long b = strtoll(argv[5], &endB, 0);
				if (endA && *endA == 0 && argv[4][0] != 0) {
					outPath = argv[5];
					want = a;
				} else if (endB && *endB == 0 && argv[5][0] != 0) {
					outPath = argv[4];
					want = b;
				}
			}
			if (want <= 0) {
				fprintf(stderr, "rawread: bad byte count (got '%s' and '%s'; need one number "
				                "and one output path)\n", argv[4], argv[5]);
				return 1;
			}
			buf = malloc((size_t)want);
			if (!buf) {
				perror("malloc");
				return 1;
			}
			rs = ntfs_raw_read_direct(argv[1], off, buf, (size_t)want);
			if (rs < 0) {
				fprintf(stderr, "rawread: offset %lld: %s\n", off, strerror(-rs));
				free(buf);
				return 1;
			}
			out = fopen(outPath, "wb");
			if (!out) {
				perror("fopen");
				free(buf);
				return 1;
			}
			fwrite(buf, 1, (size_t)rs, out);
			fclose(out);
			printf("rawread: %d of %lld bytes from offset %lld -> %s\n", rs, want, off,
			       outPath);
			free(buf);
			return rs == (int)want ? 0 : 1;
		}

		/* list:<ntfsPath>：列目录（只读直接挂载）。给"路径查不到"当诊断用 ——
		 * ntfs_pathname_to_inode 只报 ENOENT，看不出**父目录里到底有什么**；
		 * 列一遍就知道目标是"索引里根本没这个名字"还是"查找本身有问题"。 */
		if (argc == 3 && !strncmp(argv[2], "list:", 5)) {
			char *pth = unix_path(argv[2] + 5);
			char *buf = malloc(65536);
			int n;
			if (!buf) {
				perror("malloc");
				free(pth);
				return 1;
			}
			n = ntfs_list_dir_direct(argv[1], pth, buf, 65536);
			if (n < 0) {
				fprintf(stderr, "list: %s: %s\n", pth, strerror(-n));
				free(pth);
				free(buf);
				return 1;
			}
			printf("%s (%d entries):\n%s", pth, n, buf);
			free(pth);
			free(buf);
			return 0;
		}

		/* resolve:<ntfsPath>：回填**卷上真实的名字**（大小写不敏感回退）。
		 * 用途：大写 SYSTEM 查不到而真实名字是小写 system 时，把真名报出来 ——
		 * 也让调用方知道 ntfscp 该往哪个名字写。 */
		if (argc == 3 && !strncmp(argv[2], "resolve:", 8)) {
			char *pth = unix_path(argv[2] + 8);
			char *canon = malloc(2048);
			int rs;
			if (!canon) {
				perror("malloc");
				free(pth);
				return 1;
			}
			rs = ntfs_resolve_path_direct(argv[1], pth, canon, 2048);
			if (rs < 0) {
				fprintf(stderr, "resolve: %s: %s\n", pth, strerror(-rs));
				free(pth);
				free(canon);
				return 1;
			}
			printf("resolve: %s -> %s\n", pth, canon);
			free(pth);
			free(canon);
			return 0;
		}

		/* info:<ntfsPath>：把磁盘上那个 inode 的 data_size / initialized_size / runlist 打出来。
		 * 用来分辨"读回一大片零"到底是"写没落上"还是"读到了洞/未初始化区"——
		 * 读到 initialized_size 之外返回零，runlist 的 hole 也返回零。 */
		if (argc == 3 && !strncmp(argv[2], "info:", 5)) {
			char *pth = unix_path(argv[2] + 5);
			char *buf = malloc(65536);
			int rs;
			if (!buf) {
				perror("malloc");
				free(pth);
				return 1;
			}
			rs = ntfs_attr_info_direct(argv[1], pth, buf, 65536);
			if (rs < 0) {
				fprintf(stderr, "info: %s: %s\n", pth, strerror(-rs));
				free(pth);
				free(buf);
				return 1;
			}
			printf("info: %s\n%s", pth, buf);
			free(pth);
			free(buf);
			return 0;
		}

		/* record:<ntfsPath>：把磁盘上那个 inode 的 MFT 记录**原始字节/布局**倾倒出来。
		 * info: 报的是解析后的字段；当 initialized_size 出现任何合法写入者都产生不了的
		 * 值（例如 4701）时，只有看字节本身才能定案 —— 属性布局、记录序号、
		 * $STANDARD_INFORMATION 的四个时间戳都是"最后写入者"的笔迹。 */
		if (argc == 3 && !strncmp(argv[2], "record:", 7)) {
			char *pth = unix_path(argv[2] + 7);
			char *buf = malloc(65536);
			int rs;
			if (!buf) {
				perror("malloc");
				free(pth);
				return 1;
			}
			rs = ntfs_record_dump_direct(argv[1], pth, buf, 65536);
			if (rs < 0) {
				fprintf(stderr, "record: %s: %s\n", pth, strerror(-rs));
				free(pth);
				free(buf);
				return 1;
			}
			printf("record: %s\n%s", pth, buf);
			free(pth);
			free(buf);
			return 0;
		}

		/* logstate：只读探测 $LogFile 重启页版本与卷 dirty 位。
		 * RW 挂载（ntfscp -f / setdirty）在重启页 v2.0 时被 libntfs-3g 无条件拒绝
		 * （"Windows 持有缓存元数据"：fast startup / 休眠 / 掉电状态）。
		 * melt 在写回前先跑这个探针，把状态显式报给操作者。 */
		if (argc == 3 && !strcmp(argv[2], "logstate")) {
			char *buf = malloc(65536);
			int rs;
			if (!buf) {
				perror("malloc");
				return 1;
			}
			rs = ntfs_logstate_direct(argv[1], buf, 65536);
			if (rs < 0) {
				fprintf(stderr, "logstate: %s\n", strerror(-rs));
				free(buf);
				return 1;
			}
			printf("%s", buf);
			free(buf);
			return 0;
		}

		/* rm:<ntfsPath>：删一个文件（直接挂载，可写）。只给"写路径自检"清理那卷上的临时
		 * 文件用 —— 那是我们自己在卷根建的，删掉它是收尾，不是对系统文件动手。
		 * 实现放在 ntfs-3g-fuse.c（本文件不能包含 ntfs 头：layout.h 的 GUID 与
		 * <windows.h> 冲突，见文件开头的说明）。 */
		if (argc == 3 && !strncmp(argv[2], "rm:", 3)) {
			char *pth = unix_path(argv[2] + 3);
			int rs = ntfs_delete_direct(argv[1], pth);
			if (rs < 0) {
				fprintf(stderr, "rm: %s: %s\n", pth, strerror(-rs));
				free(pth);
				return 1;
			}
			printf("rm: deleted %s\n", pth);
			free(pth);
			return 0;
		}

		fprintf(stderr, "unknown command '%s' (known: setdirty, logstate, stat:<path>, list:<path>, "
		                "resolve:<path>, info:<path>, record:<path>, rm:<path>, readhead, rawread)\n", argv[2]);
	}

	if (ntfs_fuse_init()) {
		perror("failed to init the NTFS library");
		return 1;
	}
	BOOL ro = FALSE, exclusive = FALSE;
	char *p = strchr(argv[1], '*');
	if (p) {
		*p = 0;
		while (p) {
			char *opt = p + 1;
			p = strchr(p + 1, '*');
			if (p)
				*p = 0;
			if (!strcmp(opt, "ro")) ro = TRUE;
			else if (!strcmp(opt, "exclusive")) exclusive = TRUE;
			else {
				fprintf(stderr, "unknown option: %s\n", opt);
				return 1;
			}
		}
	}
	if (ntfs_open(argv[1], ro, exclusive)) {
		perror("failed to open the NTFS volume");
		return 1;
	}
	strcpy(curdir, "/");
	if (argc == 4) {
		nocli = TRUE;
		sscanf(argv[2], "%lld", &piperead);
		sscanf(argv[3], "%lld", &pipewrite);
		while (1) {
			COMMAND_PARAMS params;
			DWORD dwRead;
			if (!MyFileOP((LPFN_FILE_OP_FUNC)ReadFile, piperead, &params, sizeof(params), &dwRead) || dwRead != sizeof(params))
				break;
			char *file1 = wstrconv(params.szFile1);
			char *file2 = wstrconv(params.szFile2);
			COMMAND_RESPONSE resp = { 0 };
			resp.ret = 0;
			resp.ls_res = 0;
			wchar_t retpth[512];
			errno = 0;
			switch (params.nCmd) {
				case CMD_CD: {
					char *pth = gen_path(file1);
					strcpy(curdir, pth);
					free(pth);
					break;
				}
				case CMD_LS: {
					char *pth = gen_path(file1);
					if (resp.ret = -ntfs_fuse_readdir(pth, &resp.ls_res, list_dir_filler_getcount, 0)) {
						errno = resp.ret;
						perror("ls");
					}
					free(pth);
					break;
				}
				case CMD_CP: {
					resp.ret = ntfs_copy(file1, file2);
					break;
				}
				case CMD_CPDIR: {
					ntfs_cpdir(file1, file2);
					resp.ret = errno;
					break;
				}
				case CMD_FETCH: {
					resp.ret = ntfs_fetch(file1, file2);
					break;
				}
				case CMD_FETCHDIR: {
					ntfs_fetchdir(file1, file2);
					resp.ret = errno;
					break;
				}
				case CMD_MV: {
					char *pth1 = gen_path(file1), *pth2 = gen_path(file2);
					if (resp.ret = -ntfs_fuse_rename(pth1, pth2)) {
						errno = resp.ret;
						perror("mv");
					}
					free(pth1);
					free(pth2);
					break;
				}
				case CMD_RM: {
					char *pth = gen_path(file1);
					if (resp.ret = -ntfs_fuse_rm(pth)) {
						errno = resp.ret;
						perror("rm");
					}
					free(pth);
					break;
				}
				case CMD_RMDIR: {
					ntfs_rmdir(file1);
					resp.ret = errno;
					break;
				}
				case CMD_MKDIR: {
					char *pth = gen_path(file1);
					if (resp.ret = -ntfs_fuse_mkdir(pth, 0777)) {
						errno = resp.ret;
						perror("mkdir");
					}
					free(pth);
					break;
				}
				case CMD_TRUNC: {
					char *pth = gen_path(file1);
					if (resp.ret = -ntfs_fuse_truncate(pth, params.size)) {
						errno = resp.ret;
						perror("trunc");
					}
					free(pth);
					break;
				}
				case CMD_STAT: {
					char *pth = gen_path(file1);
					struct stat sbuf;
					memset(&sbuf, 0, sizeof(sbuf));
					if (resp.ret = -ntfs_fuse_stat(pth, &sbuf)) {
						errno = resp.ret;
						perror("stat");
						break;
					}
					const char *fn = strrchr(pth, '/');
					if (fn == NULL) fn = pth;
					else fn++;
					if (!strcmp(fn, "")) {
						int nlen = strlen(file1);
						if (nlen >= 2 && file1[nlen - 1] == '.' && file1[nlen - 2] == '.') {
							fn = "..";
						}
					}
					if (!strcmp(file1, "..") || !strcmp(file1, ".")) {
						fn = file1;
					}
					wchar_t *wfn = charconv(fn), *wfpn = charconv(pth);
					FILEITEMINFO retinfo;
					wcscpy(retinfo.szFileName, wfn);
					wcscpy(retinfo.szFullPathName, wfpn);
					ntfs_free(wfn);
					ntfs_free(wfpn);
					if (sbuf.st_mode & S_IFDIR) retinfo.nItemType = 1;
					else retinfo.nItemType = 0;
					retinfo.lFileSize = sbuf.st_size;
					retinfo.lAccessTime = sbuf.st_atime;
					retinfo.lModifyTime = sbuf.st_mtime;
					free(pth);
					resp.stat_res = retinfo;
					break;
				}
				case CMD_READLINK: {
					char *pth = gen_path(file1);
					char buf[512];
					if (resp.ret = -ntfs_fuse_readlink(pth, buf, sizeof(buf))) {
						errno = resp.ret;
						perror("readlink");
						break;
					}
					free(pth);
					resp.ls_res = 1;
					wchar_t *wpth = charconv(buf);
					wcscpy(retpth, wpth);
					ntfs_free(wpth);
					break;
				}
				case CMD_LINK: {
					char *pth1 = gen_path(file1), *pth2 = gen_path(file2);
					if (resp.ret = -ntfs_fuse_link(pth1, pth2)) {
						errno = resp.ret;
						perror("link");
					}
					free(pth1);
					free(pth2);
					break;
				}
				case CMD_SYMLINK: {
					char *pth1 = gen_path(file1), *pth2 = gen_path(file2);
					if (resp.ret = -ntfs_fuse_symlink(pth1, pth2)) {
						errno = resp.ret;
						perror("symlink");
					}
					free(pth1);
					free(pth2);
					break;
				}
				case CMD_UNLINK: {
					char *pth = gen_path(file1);
					if (resp.ret = -ntfs_fuse_unlink(pth)) {
						errno = resp.ret;
						perror("unlink");
					}
					free(pth);
					break;
				}
				case CMD_EXIT:
					ntfs_close();
					return 0;
					break;
				default:
					resp.ret = -1;
					break;
			}
			wchar_t *wcurdir = charconv(curdir);
			wcscpy(resp.curdir, wcurdir);
			ntfs_free(wcurdir);
			DWORD dwWrite;
			MyFileOP((LPFN_FILE_OP_FUNC)WriteFile, pipewrite, &resp, sizeof(resp), &dwWrite);
			if (resp.ret == 0) {
				if (params.nCmd == CMD_LS) {
					char *pth = gen_path(file1);
					file_count = 0;
					errno = 0;
					if (resp.ret = -ntfs_fuse_readdir(pth, &resp.ls_res, list_dir_filler, 0)) {
						errno = resp.ret;
						perror("ls");
					}
					if (file_count < resp.ls_res) {
						int sz = sizeof(wchar_t) * 512 * (resp.ls_res - file_count);
						wchar_t *tmp = malloc(sz);
						memset(tmp, 0, sz);
						MyFileOP((LPFN_FILE_OP_FUNC)WriteFile, pipewrite, tmp, sz, &dwWrite);
					}
					free(pth);
				} else if (params.nCmd == CMD_READLINK) {
					DWORD dwWrite;
					MyFileOP((LPFN_FILE_OP_FUNC)WriteFile, pipewrite, retpth, sizeof(retpth), &dwWrite);
				}
			}
			ntfs_free(file1);
			ntfs_free(file2);
		}
		ntfs_close();
		return 0;
	}
	char cmd[4096];
	while (!feof(stdin)) {
		printf("%s $ ", curdir);
		gets(cmd);
		interrupt = FALSE;
		errno = 0;
		int cmd_argc = 0;
		char **cmd_argv = parse_cmd(cmd, &cmd_argc);
		if (cmd_argc < 1);
		else if (!strcmp(cmd_argv[0], "help")) cmd_help(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "cd")) cmd_cd(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "ls")) cmd_ls(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "cp")) cmd_cp(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "mv")) cmd_mv(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "rm")) cmd_rm(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "trunc")) cmd_trunc(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "readlink")) cmd_readlink(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "link")) cmd_link(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "symlink")) cmd_symlink(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "unlink")) cmd_unlink(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "mkdir")) cmd_mkdir(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "fetch")) cmd_fetch(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "cat")) cmd_cat(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "cpdir")) cmd_cpdir(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "rmdir")) cmd_rmdir(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "fetchdir")) cmd_fetchdir(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "stat")) cmd_stat(cmd_argc, cmd_argv);
		else if (!strcmp(cmd_argv[0], "exit")) break;
		else cmd_help(1, cmd_argv);
		if (cmd_argv) GlobalFree(cmd_argv);
	}
	ntfs_close();
	return 0;
}