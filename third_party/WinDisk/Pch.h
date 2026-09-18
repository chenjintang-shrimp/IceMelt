#pragma once

#include <ntifs.h>
#include <srb.h>
#include <scsi.h>
#include "Public.h"

/* 单次 DbgPrint：让 DebugView 的 include filter("WinDisk") 只产出完整的一行，
 * 避免"tag 与消息体被分别当成两个调试事件"导致消息行（不含 WinDisk 关键字）被
 * 过滤掉到没的状态。 */
#define LogInfo(fmt, ...) DbgPrint("WinDisk [INFO] [%S] " fmt, __FUNCTIONW__, __VA_ARGS__)
#define LogWarn(fmt, ...) DbgPrint("WinDisk [WARN] [%S] " fmt, __FUNCTIONW__, __VA_ARGS__)
#define LogErr(fmt, ...) DbgPrint("WinDisk [ERR] [%S] " fmt, __FUNCTIONW__, __VA_ARGS__)

#define TRY_START __try {
#define TRY_END(RetStatus) } __except (1) { LogErr("Unknown error: 0x%.8x", GetExceptionCode()); }; return RetStatus;
#define TRY_END_NOSTATUS } __except (1) { LogErr("Unknown error: 0x%.8x", GetExceptionCode()); }; return;
