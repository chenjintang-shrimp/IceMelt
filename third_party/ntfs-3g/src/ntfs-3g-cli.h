#pragma once
#include <time.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

enum {
	CMD_CD = 0,
	CMD_LS = 1,
	CMD_CP = 2,
	CMD_CPDIR = 3,
	CMD_FETCH = 4,
	CMD_FETCHDIR = 5,
	CMD_MV = 6,
	CMD_RM = 7,
	CMD_RMDIR = 8,
	CMD_MKDIR = 9,
	CMD_TRUNC = 10,
	CMD_STAT = 11,
	CMD_READLINK = 12,
	CMD_LINK = 13,
	CMD_SYMLINK = 14,
	CMD_UNLINK = 15,
	CMD_EXIT = 16
};

#pragma pack(push, 1)
typedef struct {
	wchar_t szFileName[MAX_PATH];
	wchar_t szFullPathName[512];
	int nItemType; // 0: file, 1: directory
	__int64 lFileSize;
	time_t lAccessTime;
	time_t lModifyTime;
} FILEITEMINFO, *PFILEITEMINFO;

typedef struct {
	unsigned char nCmd;
	wchar_t szFile1[512];
	wchar_t szFile2[512];
	__int64 size;
} COMMAND_PARAMS;

typedef struct {
	int ret; // 0: success, <0: errno, >0: otherwise
	wchar_t curdir[512];
	int ls_res; // ls: file item count
	FILEITEMINFO stat_res;
} COMMAND_RESPONSE;
#pragma pack(pop)
