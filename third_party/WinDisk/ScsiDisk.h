#pragma once

#include "Pch.h"
#include "DataList.h"

const int g_ulBytesPerSector = 512;

NTSTATUS ScsiReadWriteDisk(PDEVICE_OBJECT pDevObj, BOOLEAN bIsRead, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount);
NTSTATUS ScsiReadDisk(PDEVICE_OBJECT pDevObj, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount);
NTSTATUS ScsiWriteDisk(PDEVICE_OBJECT pDevObj, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount, int nRetryCount = 8);
NTSTATUS ScsiWriteDisk(PDATA_LIST_ENTRY pDevObjs, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount, int nRetryCount = 8);
