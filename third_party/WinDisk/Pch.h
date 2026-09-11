#pragma once

#include <ntifs.h>
#include <srb.h>
#include <scsi.h>
#include "Public.h"

#define LogInfo(...) (DbgPrint("WinDisk [INFO] [%S] ", __FUNCTIONW__), DbgPrint(__VA_ARGS__))
#define LogWarn(...) (DbgPrint("WinDisk [WARN] [%S] ", __FUNCTIONW__), DbgPrint(__VA_ARGS__))
#define LogErr(...) (DbgPrint("WinDisk [ERR] [%S] ", __FUNCTIONW__), DbgPrint(__VA_ARGS__))

#define TRY_START __try {
#define TRY_END(RetStatus) } __except (1) { LogErr("Unknown error: 0x%.8x", GetExceptionCode()); }; return RetStatus;
#define TRY_END_NOSTATUS } __except (1) { LogErr("Unknown error: 0x%.8x", GetExceptionCode()); }; return;
