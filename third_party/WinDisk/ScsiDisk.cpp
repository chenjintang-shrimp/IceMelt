#include "ScsiDisk.h"

/* Main.cpp 的 SetTargetDisk→ResolveSrbBypass 在发现端口驱动 IRP_MJ_SCSI 被钩
 * （DfDiskLo 式 dispatch hook）时，从 hooker 的挂钩记录里救回受害驱动的原始
 * 处理函数存到这里。直调它 = 真正访问磁盘；IoCallDriver = 先进钩子的账本。
 *
 * 调用形态与 IoCallDriver 完全一致（内部本来就是这个函数指针的间接调用），
 * 所以不需要换表、不触碰冰点全局状态、不留任何可检痕迹。 */
extern PVOID g_BypassSrbHandler;
typedef NTSTATUS (*PDRIVER_DISPATCH_FN)(PDEVICE_OBJECT, PIRP);

NTSTATUS ScsiReadWriteDiskCompletion(PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID Context)
{
	if (pIrp->UserIosb) {
		pIrp->UserIosb->Information = pIrp->IoStatus.Information;
		pIrp->UserIosb->Status = pIrp->IoStatus.Status;
	}

	if (Context != NULL && pIrp->MdlAddress != NULL)
	{
		// 与 ScsiReadWriteDiskInternal 中的 MmProbeAndLockPages 成对。
		// 若换成 MmBuildMdlForNonPagedPool 就不能在这里解锁（会 0x4E）。
		MmUnlockPages(pIrp->MdlAddress);
		IoFreeMdl(pIrp->MdlAddress);
		pIrp->MdlAddress = NULL;
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

	/* IoCallDriver 是按 DeviceObject->DriverObject->MajorFunction[IRP_MJ_SCSI]
	 * 取函数指针调用的。目标设备为空、或它不处理 IRP_MJ_SCSI，就等价于跳到地址 0，
	 * 报出来就是 0xD1 (Arg1=0 Arg2=2 Arg3=8 Arg4=0)。在这里拦住并说明原因，
	 * 比让内核去执行空地址好得多。 */
	if (pDevObj == NULL || pDevObj->DriverObject == NULL ||
		pDevObj->DriverObject->MajorFunction[IRP_MJ_SCSI] == NULL)
	{
		LogWarn("SCSI target is unusable (dev=%p): no IRP_MJ_SCSI handler\n", pDevObj);
		return STATUS_DEVICE_NOT_READY;
	}

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
	pSrb->ScsiStatus = SCSISTAT_GOOD;
	pSrb->QueueAction = 0;

	pSrb->SenseInfoBuffer = pSenseData;
	pSrb->SenseInfoBufferLength = sizeof(SENSE_DATA);

	pSrb->DataBuffer = lpDataBuff;
	pSrb->DataTransferLength = ulSecCount * g_ulBytesPerSector;
	pSrb->QueueSortKey = ulSectorPos;

	// 方向和自动 sense 选项都属于 SrbFlags。RMW/写后验证不能使用
	// 适配器缓存，否则读可能返回旧扇区内容，随后把旧数据重新写回去。
	pSrb->SrbFlags = SRB_FLAGS_DISABLE_AUTOSENSE;
	if (bIsRead)
	{
		pSrb->SrbFlags |= SRB_FLAGS_DATA_IN;
	}
	else
	{
		pSrb->SrbFlags |= SRB_FLAGS_DATA_OUT;
	}

	pSrb->CdbLength = 0x0A;
	pSrb->Cdb[0] = bIsRead ? SCSIOP_READ : SCSIOP_WRITE;
	pSrb->Cdb[1] = 0;
	pSrb->Cdb[2] = (UCHAR)(ulSectorPos >> 0x18) & 0xFF;
	pSrb->Cdb[3] = (UCHAR)(ulSectorPos >> 0x10) & 0xFF;
	pSrb->Cdb[4] = (UCHAR)(ulSectorPos >> 0x08) & 0xFF;
	pSrb->Cdb[5] = (UCHAR)ulSectorPos;
	pSrb->Cdb[7] = (UCHAR)(ulSecCount >> 0x08);
	pSrb->Cdb[8] = (UCHAR)ulSecCount;

	LogInfo("SCSI %s request: LBA=%lu sectors=%lu bytes=%lu buffer=%p flags=0x%.8X CDB=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
		bIsRead ? "READ" : "WRITE", ulSectorPos, ulSecCount,
		(unsigned long)(ulSecCount * g_ulBytesPerSector), lpDataBuff,
		(unsigned long)pSrb->SrbFlags, pSrb->Cdb[0], pSrb->Cdb[1],
		pSrb->Cdb[2], pSrb->Cdb[3], pSrb->Cdb[4], pSrb->Cdb[5],
		pSrb->Cdb[6], pSrb->Cdb[7], pSrb->Cdb[8], pSrb->Cdb[9]);
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

	/* lpDataBuff 来自非分页池，但 MDL 必须与完成例程中的 MmUnlockPages 成对：
	 * MmBuildMdlForNonPagedPool 生成的 MDL 一旦被 MmUnlockPages 处理就会破坏 PFN
	 * 数据库（0x4E PFN_LIST_CORRUPT），所以这里两处必须同进同出。
	 * 访问方向按 CPU 侧行为表达：读盘时数据写入调用方缓冲区（IoWriteAccess），
	 * 写盘时从缓冲区读出（IoReadAccess）。原代码这两者正好写反了。 */
	MmProbeAndLockPages(pMdl, KernelMode, bIsRead ? IoWriteAccess : IoReadAccess);

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

	/* 必须注册完成例程。pIrpSp 是自建 IRP 的目标栈单元：IoSetCompletionRoutine 会把
	 * SL_INVOKE_ON_* 与 CompletionRoutine 一起写在这里，而 IRP 完成时 I/O 管理器正是
	 * 按这两个字段回调。只写 Control 不写函数指针 = 完成时调用地址 0，
	 * 即 0xD1 (Arg1=0 Arg2=2 Arg3=8 Arg4=0)。注意注册之后不能再用 pIrpSp->Control
	 * 覆盖这些标志位。 */
	IoSetCompletionRoutine(pIrp, ScsiReadWriteDiskCompletion, pSrb, TRUE, TRUE, TRUE);

	/* 旁路：端口 PDO 的分派被冰品类钩住时,不走 IoCallDriver(那张表已被人换过),
	 * 直接调 hooker 记录里存下的受害驱动原始 dispatch。签名与 IoCallDriver 相同。
	 *
	 * 但 IofCallDriver 在调 dispatch 之前会做 CurrentLocation/CurrentStackLocation
	 * 同降一格（这就是 IoSetNextIrpStackLocation 宏体）——被调方读
	 * [IRP+0xB8] 时拿到的才是我们刚填的那一格。DfDiskLo 的 stub 之所以能给
	 * r14 传对了，是因为它本身就是经 IoCallDriver 进来的、栈已降格；
	 * 我们直调漏掉这一步 = 被调方读到未填的垃圾槽(上轮 storport +0x17CB
	 * 的 0x3B C0000005 即此)。直调前必须补这一刀。 */
	if (g_BypassSrbHandler)
	{
		IoSetNextIrpStackLocation(pIrp);
		ntStatus = ((PDRIVER_DISPATCH_FN)g_BypassSrbHandler)(pDevObj, pIrp);
	}
	else
	{
		ntStatus = IoCallDriver(pDevObj, pIrp);
	}
	if (ntStatus == STATUS_PENDING)
	{
		KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
		ntStatus = IoSB.Status;
	}
	LogInfo("IoCallDriverStatus: 0x%.8x (via %s)\n", ntStatus,
		g_BypassSrbHandler ? "bypass" : "dispatch");
	LogInfo("SrbStatus: 0x%.8x\n", pSrb->SrbStatus);
	LogInfo("ScsiStatus: 0x%.8x\n", pSrb->ScsiStatus);
	LogInfo("IoSB.Status: 0x%.8x Information=%llu Expected=%lu\n",
		IoSB.Status, (unsigned long long)IoSB.Information,
		(unsigned long)(ulSecCount * g_ulBytesPerSector));
	if (!NT_SUCCESS(ntStatus))
		goto Cleanup;
	if (pSrb->SrbStatus != SRB_STATUS_SUCCESS)
	{
		ntStatus = 0xC1000001L + pSrb->SrbStatus;
		goto Cleanup;
	}
	if (pSrb->ScsiStatus != SCSISTAT_GOOD)
	{
		ntStatus = 0xC2000001L + pSrb->ScsiStatus;
		goto Cleanup;
	}
	if (IoSB.Information != ulSecCount * g_ulBytesPerSector)
	{
		LogWarn("Short SCSI transfer: %llu/%lu bytes\n",
			(unsigned long long)IoSB.Information,
			(unsigned long)(ulSecCount * g_ulBytesPerSector));
		ntStatus = STATUS_DEVICE_DATA_ERROR;
	}

Cleanup:
	if (pSrb != NULL && pSrb->SenseInfoBuffer && pSrb->SenseInfoBuffer != pSenseData)
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
	if (IsListEmpty(&pDevObjs->DataList))
		return STATUS_DEVICE_NOT_READY;

	PLIST_ENTRY pTarget = pDevObjs->DataList.Flink;
	while (pTarget != &pDevObjs->DataList)
	{
		PDATA_LIST pDataTarget = CONTAINING_RECORD(pTarget, DATA_LIST, ListEntry);
		PDEVICE_OBJECT pDevObj = *((PDEVICE_OBJECT*)pDataTarget->Buffer);
		const NTSTATUS status = ScsiWriteDisk(pDevObj, ulSectorPos, lpDataBuff, ulSecCount, nRetryCount);
		if (!NT_SUCCESS(status))
		{
			LogWarn("Write failed on one target device: 0x%.8X\\n", status);
			return status;
		}
		pTarget = pTarget->Flink;
	}
	return STATUS_SUCCESS;
}
