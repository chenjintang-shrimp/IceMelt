#include "DataList.h"

BOOLEAN List_Add(
	PDATA_LIST_ENTRY pListEntry,
	PVOID Buffer,
	SIZE_T Size,
	BOOLEAN IsTail
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	PDATA_LIST pAddList = (PDATA_LIST)ExAllocatePoolWithTag(NonPagedPool, sizeof(DATA_LIST), 'TSIL');
	if (pAddList)
	{
		pAddList->Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, 'FBTI');
		if (pAddList->Buffer)
		{
			pAddList->Size = Size;
			memcpy(pAddList->Buffer, Buffer, Size);
			if (IsTail)
				ExInterlockedInsertTailList(&pListEntry->DataList, &pAddList->ListEntry, &pListEntry->Lock);
			else
				ExInterlockedInsertHeadList(&pListEntry->DataList, &pAddList->ListEntry, &pListEntry->Lock);
			bRet = TRUE;
		}
		else
			ExFreePoolWithTag(pAddList, 'TSIL');
	}
	return bRet;

	TRY_END(bRet)
}

BOOLEAN List_Show(
	CONST PDATA_LIST_ENTRY pListEntry
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	if (!IsListEmpty(&pListEntry->DataList))
	{
		PLIST_ENTRY pTarget = pListEntry->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pListEntry->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			LogInfo("[LIST] DataPointer:%p ,DataSize:%I64u ,Data:", pDataTarget, pDataTarget->Size);
			for (SIZE_T i = 0; i < pDataTarget->Size; i++)
				DbgPrint("%.2X ", ((PUCHAR)pDataTarget->Buffer)[i]);
			DbgPrint("\n");
			pTarget = pTarget->Flink;
			bRet = TRUE;
		}
	}
	return bRet;

	TRY_END(bRet)
}

BOOLEAN List_Check(
	CONST PDATA_LIST_ENTRY pListEntry,
	PVOID Buffer,
	SIZE_T Size/*,
	BOOLEAN IsUserSize*/
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	if (!IsListEmpty(&pListEntry->DataList))
	{
		SIZE_T _Size = 0;
		PLIST_ENTRY pTarget = pListEntry->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pListEntry->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			/*
			if (IsUserSize)
				_Size = Size;
			else
				_Size = pDataTarget->Size;
			*/
			if (Size >= pDataTarget->Size)
				_Size = pDataTarget->Size;
			else
				_Size = Size;
			if (!memcmp(pDataTarget->Buffer, Buffer, _Size))
			{
				bRet = TRUE;
				break;
			}
			pTarget = pTarget->Flink;
		}
	}
	return bRet;

	TRY_END(bRet)
}

BOOLEAN List_Delete(
	PDATA_LIST_ENTRY pListEntry,
	PVOID Buffer,
	SIZE_T Size,
	BOOLEAN IsUserSize
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	if (!IsListEmpty(&pListEntry->DataList))
	{
		SIZE_T _Size = 0;
		PLIST_ENTRY pTarget = pListEntry->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pListEntry->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			if (IsUserSize)
				_Size = Size;
			else
				_Size = pDataTarget->Size;
			if (!memcmp(pDataTarget->Buffer, Buffer, _Size))
			{
				KIRQL OldIrql = { 0 };
				KeAcquireSpinLock(&pListEntry->Lock, &OldIrql);
				bRet = RemoveEntryList(&pListEntry->DataList);
				KeReleaseSpinLock(&pListEntry->Lock, OldIrql);
				break;
			}
			pTarget = pTarget->Flink;
		}
	}

	TRY_END(bRet)
}

BOOLEAN List_DeleteAll(
	PDATA_LIST_ENTRY pListEntry
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	PLIST_ENTRY pTarget = NULL;
	PDATA_LIST pDataTarget = NULL;
	while (!IsListEmpty(&pListEntry->DataList))
	{
		pTarget = ExInterlockedRemoveHeadList(&pListEntry->DataList, &pListEntry->Lock);
		pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
		ExFreePoolWithTag(pDataTarget->Buffer, 'FBTI');
		ExFreePoolWithTag(pDataTarget, 'TSIL');
		bRet = TRUE;
	}
	return bRet;

	TRY_END(bRet)
}

PDATA_LIST List_GetHeadItem(
	CONST PDATA_LIST_ENTRY pListEntry
)
{
	TRY_START

	return CONTAINING_RECORD((pListEntry)->DataList.Flink, DATA_LIST, ListEntry);

	TRY_END(NULL)
}

PDATA_LIST List_GetTailItem(
	CONST PDATA_LIST_ENTRY pListEntry
)
{
	TRY_START

	if (!IsListEmpty(&pListEntry->DataList))
	{
		PLIST_ENTRY pTarget = pListEntry->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pListEntry->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			pTarget = pTarget->Flink;
			if (pTarget == &pListEntry->DataList)
			{
				return pDataTarget;
			}
		}
	}
	return NULL;

	TRY_END(NULL)
}

BOOLEAN List_Visit(
	CONST PDATA_LIST_ENTRY pListEntry,
	const PDATA_LIST_VISIT_CALLBACK pVisitCallback
)
{
	BOOLEAN bRet = FALSE;
	TRY_START

	if (!IsListEmpty(&pListEntry->DataList))
	{
		PLIST_ENTRY pTarget = pListEntry->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pListEntry->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			if (!pVisitCallback(pDataTarget->Buffer, pDataTarget->Size))
			{
				break;
			}
			pTarget = pTarget->Flink;
			bRet = TRUE;
		}
	}
	return bRet;

	TRY_END(bRet)
}