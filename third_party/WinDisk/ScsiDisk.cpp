#include "ScsiDisk.h"

NTSTATUS ScsiReadWriteDiskCompletion(PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID Context)
{
	pIrp->UserIosb->Information = pIrp->IoStatus.Information;
	pIrp->UserIosb->Status = pIrp->IoStatus.Status;

	if (Context != NULL && pIrp->MdlAddress != NULL)
	{
		MmUnlockPages(pIrp->MdlAddress);
		IoFreeMdl(pIrp->MdlAddress);
	}

	KeSetEvent(pIrp->UserEvent, 0, FALSE);
	IoFreeIrp(pIrp);

	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS ScsiReadWriteDiskInternal(PDEVICE_OBJECT pDevObj, BOOLEAN bIsRead, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount)
{
	NTSTATUS ntStatus = STATUS_SUCCESS;
	PSCSI_REQUEST_BLOCK pSrb = NULL;
	PSENSE_DATA pSenseData = NULL;
	KEVENT Event;
	PIRP pIrp = NULL;
	PMDL pMdl = NULL;
	IO_STATUS_BLOCK IoSB;

	pSrb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK), 'WK');

	if (pSrb == NULL)
	{
		LogWarn("Could not allocate SRB memory\n");
		ntStatus = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}

	pSenseData = (PSENSE_DATA)ExAllocatePoolWithTag(NonPagedPool, sizeof(SENSE_DATA), 'WK');

	if (pSenseData == NULL)
	{
		LogWarn("Could not allocate SenseData memory\n");
		ntStatus = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}

	RtlZeroMemory(pSrb, sizeof(SCSI_REQUEST_BLOCK));
	RtlZeroMemory(pSenseData, sizeof(SENSE_DATA));

	pSrb->Length = sizeof(SCSI_REQUEST_BLOCK);
	pSrb->Function = SRB_FUNCTION_EXECUTE_SCSI;

	pSrb->NextSrb = NULL;
	pSrb->LinkTimeoutValue = -1;
	pSrb->SrbStatus = SRB_STATUS_PENDING;
	pSrb->ScsiStatus = SRB_STATUS_PENDING;
	pSrb->QueueAction = SRB_FLAGS_DISABLE_AUTOSENSE;

	pSrb->SenseInfoBuffer = pSenseData;
	pSrb->SenseInfoBufferLength = sizeof(SENSE_DATA);

	pSrb->DataBuffer = lpDataBuff;
	pSrb->DataTransferLength = ulSecCount * g_ulBytesPerSector;
	pSrb->QueueSortKey = ulSectorPos;

	pSrb->SrbFlags |= SRB_FLAGS_DISABLE_AUTOSENSE;
	pSrb->SrbFlags |= SRB_FLAGS_NO_QUEUE_FREEZE | SRB_FLAGS_BYPASS_FROZEN_QUEUE;
	if (bIsRead)
	{
		pSrb->SrbFlags |= SRB_FLAGS_DATA_IN;
		pSrb->SrbFlags |= SRB_FLAGS_ADAPTER_CACHE_ENABLE;
	}
	else
	{
		pSrb->SrbFlags |= SRB_FLAGS_DATA_OUT;
	}

	pSrb->CdbLength = 0x0A;
	pSrb->Cdb[0] = bIsRead ? SCSIOP_READ : SCSIOP_WRITE;
	pSrb->Cdb[1] = pSrb->Cdb[1] & 0x1F | 0x80;
	pSrb->Cdb[2] = (UCHAR)(ulSectorPos >> 0x18) & 0xFF;
	pSrb->Cdb[3] = (UCHAR)(ulSectorPos >> 0x10) & 0xFF;
	pSrb->Cdb[4] = (UCHAR)(ulSectorPos >> 0x08) & 0xFF;
	pSrb->Cdb[5] = (UCHAR)ulSectorPos;
	pSrb->Cdb[7] = (UCHAR)(ulSecCount >> 0x08);
	pSrb->Cdb[8] = (UCHAR)ulSecCount;

	KeInitializeEvent(&Event, NotificationEvent, FALSE);
	pIrp = IoAllocateIrp(pDevObj->StackSize, FALSE);

	if (pIrp == NULL)
	{
		LogWarn("Could not allocate IRP\n");
		ntStatus = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}

	pMdl = IoAllocateMdl((PVOID)lpDataBuff, ulSecCount * g_ulBytesPerSector, FALSE, FALSE, pIrp);

	if (pMdl == NULL)
	{
		IoFreeIrp(pIrp);
		pIrp = NULL;
		LogWarn("Could not allocate MDL\n");
		ntStatus = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}

	MmProbeAndLockPages(pMdl, KernelMode, bIsRead ? IoReadAccess : IoWriteAccess);

	RtlZeroMemory(&IoSB, sizeof(IO_STATUS_BLOCK));
	pIrp->UserIosb = &IoSB;
	RtlZeroMemory(&pIrp->IoStatus, sizeof(IO_STATUS_BLOCK));
	pIrp->UserEvent = &Event;
	pIrp->Cancel = FALSE;
	pIrp->CancelRoutine = NULL;
	pIrp->MdlAddress = pMdl;
	pIrp->AssociatedIrp.SystemBuffer = NULL;
	pIrp->Flags = IRP_NOCACHE | IRP_SYNCHRONOUS_API;
	pIrp->RequestorMode = KernelMode;
	pIrp->Tail.Overlay.Thread = PsGetCurrentThread();

	pSrb->OriginalRequest = pIrp;

	PIO_STACK_LOCATION pIrpSp = IoGetNextIrpStackLocation(pIrp);

	pIrpSp->DeviceObject = pDevObj;
	pIrpSp->MajorFunction = IRP_MJ_SCSI;
	pIrpSp->Parameters.Scsi.Srb = pSrb;
	pIrpSp->Control = SL_INVOKE_ON_CANCEL | SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR;

	IoSetCompletionRoutine(pIrp, ScsiReadWriteDiskCompletion, pSrb, TRUE, TRUE, TRUE);

	ntStatus = IoCallDriver(pDevObj, pIrp);

	if (ntStatus == STATUS_PENDING)
	{
		KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
		ntStatus = STATUS_SUCCESS;
	}
	LogInfo("IoCallDriverStatus: 0x%.8x\n", ntStatus);
	LogInfo("SrbStatus: 0x%.8x\n", pSrb->SrbStatus);
	LogInfo("ScsiStatus: 0x%.8x\n", pSrb->ScsiStatus);
	LogInfo("IoSB.Status: 0x%.8x\n", IoSB.Status);
	if (NT_SUCCESS(ntStatus))
	{
		if (pSrb->SrbStatus == SRB_STATUS_SUCCESS)
		{
			if (pSrb->ScsiStatus == SCSISTAT_GOOD)
			{
				ntStatus = STATUS_SUCCESS;
			}
			else
			{
				ntStatus = 0xC2000001L + pSrb->ScsiStatus;
			}
		}
		else
		{
			ntStatus = 0xC1000001L + pSrb->SrbStatus;
		}
	}

Cleanup:
	if (pSrb->SenseInfoBuffer && pSrb->SenseInfoBuffer != pSenseData)
	{
		ExFreePool(pSrb->SenseInfoBuffer);
	}

	if (pSrb != NULL)
	{
		ExFreePool(pSrb);
	}

	if (pSenseData != NULL)
	{
		ExFreePool(pSenseData);
	}

	return ntStatus;
}

NTSTATUS ScsiReadWriteDisk(PDEVICE_OBJECT pDevObj, BOOLEAN bIsRead, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount)
{
	ULONG CurrentPos = 0;
	NTSTATUS status = STATUS_SUCCESS;
	while (CurrentPos < ulSecCount)
	{
		ULONG SecCount = min(ulSecCount - CurrentPos, 8);
		status = ScsiReadWriteDiskInternal(pDevObj, bIsRead, ulSectorPos + CurrentPos, ((PUCHAR)lpDataBuff) + CurrentPos * 512, SecCount);
		if (!NT_SUCCESS(status))
			return status;
		CurrentPos += SecCount;
	}
	return status;
}

NTSTATUS ScsiReadDisk(PDEVICE_OBJECT pDevObj, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount)
{
	return ScsiReadWriteDisk(pDevObj, TRUE, ulSectorPos, lpDataBuff, ulSecCount);
}

NTSTATUS ScsiWriteDisk(PDEVICE_OBJECT pDevObj, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount, int nRetryCount)
{
	NTSTATUS WriteStatus;
	do
	{
		WriteStatus = ScsiReadWriteDisk(pDevObj, FALSE, ulSectorPos, lpDataBuff, ulSecCount);
	} while (!NT_SUCCESS(WriteStatus) && (nRetryCount--));
	return WriteStatus;
}

NTSTATUS ScsiWriteDisk(PDATA_LIST_ENTRY pDevObjs, ULONG ulSectorPos, PVOID lpDataBuff, ULONG ulSecCount, int nRetryCount)
{
	NTSTATUS WriteStatus = STATUS_UNSUCCESSFUL;
	if (!IsListEmpty(&pDevObjs->DataList))
	{
		PLIST_ENTRY pTarget = pDevObjs->DataList.Flink;
		PDATA_LIST pDataTarget = NULL;
		while (pTarget != &pDevObjs->DataList)
		{
			pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
			PDEVICE_OBJECT pDevObj = *((PDEVICE_OBJECT*)pDataTarget->Buffer);
			if (NT_SUCCESS(ScsiWriteDisk(pDevObj, ulSectorPos, lpDataBuff, ulSecCount, nRetryCount)))
				WriteStatus = STATUS_SUCCESS;
			pTarget = pTarget->Flink;
		}
	}
	return WriteStatus;
}
