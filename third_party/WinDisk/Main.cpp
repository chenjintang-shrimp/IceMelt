#include "ScsiDisk.h"
#include "DataList.h"
#include "FileUnlock.h"

/* 这套 Win7 WDK 头没有导出的 MmIsAddressValid 原型（NT 导出，老驱动通吃）。
 * 手动声明；语义：虚址当前可安全解引用（映射可用）返回 TRUE。 */
extern "C" NTKERNELAPI BOOLEAN NTAPI MmIsAddressValid(_In_ PVOID VirtualAddress);

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath);
VOID DriverUnload(IN PDRIVER_OBJECT DriverObject);

PDEVICE_OBJECT pDevice;
UNICODE_STRING DeviceName;
UNICODE_STRING SymLinkName;

DATA_LIST_ENTRY TargetDisks = { 0 };
PDEVICE_OBJECT TargetDisk = NULL;

/* 全局符号，勿进命名空间 —— ScsiDisk.cpp 要 extern 引用它做直调。
 * 非空 = 端口 PDO 的 IRP_MJ_SCSI 被钩、已从 hooker 的记录里救回真身。 */
PVOID g_BypassSrbHandler = NULL;

namespace FSDAntiHook
{
	typedef struct _LDR_DATA_TABLE_ENTRY {
		LIST_ENTRY InLoadOrderLinks;
		LIST_ENTRY InMemoryOrderLinks;
		LIST_ENTRY InInitializationOrderLinks;
		PVOID DllBase;
		PVOID EntryPoint;
		ULONG SizeOfImage;
		UNICODE_STRING FullDllName;
		UNICODE_STRING BaseDllName;
		union {
			ULONG FlagGroup;
			ULONG Flags;
		};
	} LDR_DATA_TABLE_ENTRY64, *PLDR_DATA_TABLE_ENTRY64;

	/* 沿 InLoadOrderLinks 找到容纳 addr 的内核模块（所有已加载驱动都在这条
	 * 链上；入口就是任意 DriverObject->DriverSection 所指的本模块表项）。
	 * 找不到返回 NULL —— 指针落在任何模块之外（比如被换表到某个未登记的壳区）。 */
	PLDR_DATA_TABLE_ENTRY64 FindModuleByAddress(PVOID addr, PLDR_DATA_TABLE_ENTRY64 entry)
	{
		PLIST_ENTRY head = &entry->InLoadOrderLinks;
		for (PLIST_ENTRY p = head->Flink; p != head; p = p->Flink)
		{
			PLDR_DATA_TABLE_ENTRY64 e =
				CONTAINING_RECORD(p, LDR_DATA_TABLE_ENTRY64, InLoadOrderLinks);
			if ((ULONG_PTR)addr >= (ULONG_PTR)e->DllBase &&
				(ULONG_PTR)addr < (ULONG_PTR)e->DllBase + e->SizeOfImage)
				return e;
		}
		return NULL;
	}

	/* 钩子已坐实时：把 hooker 映像里所有"值落在受害驱动映像区间内"的 qword 全部
	 * 捞出来。冰点类驱动必须保存受害驱动的原始 dispatch 以便对自己 / 解冻态放行，
	 * 抄件就藏在它自己的 .data（或它的 import/全局区）里 —— 这里列出的就是
	 * 头号嫌疑格。读取原值后按指针直调即可绕过钩子，无需换表，也不惊动看门狗。 */
	void ScanForSavedOriginal(PLDR_DATA_TABLE_ENTRY64 hooker,
	                          PLDR_DATA_TABLE_ENTRY64 victim)
	{
		const UCHAR* img = (const UCHAR*)hooker->DllBase;
		const ULONG_PTR vBase = (ULONG_PTR)victim->DllBase;
		const ULONG_PTR vEnd = vBase + victim->SizeOfImage;
		if (!img || !hooker->SizeOfImage)
			return;
		for (ULONG_PTR off = 0; off + sizeof(ULONG_PTR) <= hooker->SizeOfImage; off += sizeof(ULONG_PTR))
		{
			/* SizeOfImage 把 INIT 等已丢弃段也计入，而那些页面加载后早被回收 —
			 * 顺着 SizeOfImage 扫到映像尾部必然 PAGE_FAULT_IN_NONPAGED_AREA。
			 * 逐页探活，第一个不可映射点即止步；截断点本身也是情报：
			 * saved-original 只可能藏在它之前的 .data 里。 */
			if (!MmIsAddressValid((PVOID)(img + off)))
			{
				LogWarn("  %wZ image unmapped past +0x%llX (size 0x%X includes discarded pages); "
				        "stopping scan here\n",
				        &hooker->BaseDllName, (unsigned long long)off, hooker->SizeOfImage);
				break;
			}
			const ULONG_PTR v = *(const ULONG_PTR*)(img + off);
			if (v >= vBase && v < vEnd)
			{
				LogWarn("  saved-original 嫌疑: %wZ+0x%llX = %p\n",
					&hooker->BaseDllName, (unsigned long long)off, (PVOID)v);
			}
		}
	}

	/* DfDiskLo（DeepFreeze 系）的挂钩记录布局 —— 来自对该版本
	 * (SizeOfImage=0xD000, 2021-07-15) 的静态分析：
	 *   全局 DfBase+0x153b0 存指针 P；P+0x00 是 LIST_ENTRY 表头、P+0x10 是自旋锁。
	 *   每条节点记录的链节在 node+0x100，node+0x00 是被钩的 DEVICE_OBJECT (PDO)，
	 *   node+0x98 是被存下的原始 IRP_MJ_SCSI dispatch（DfDiskLo 自己的解析器
	 *   0x1303c 就是这样按 PDO 查表的）。
	 * 冰点升级换版本时这些偏移都会变 —— 出缓之前必须打印并验证。 */
	/* 注意单位：objdump 里 rip 相对寻址的注释给的是**带 ImageBase 的 VA**
	 * （样本 ImageBase=0x10000），活动映像要用的是 **RVA**（= VA - ImageBase）。
	 * 之前误把 0x153B0(VA) 直接加在活动基址上，等于戳到映像外未映射页，
	 * 以 0x50 收场 —— 这个错误不再犯第二次（崩点地址反推更是如此：
	 * faultAddr - 0x153B0 恰好等于该次启动的 DfBase）。 */
	static const ULONG_PTR kDfHookListGlobal = 0x53b0;   /* RVA */
	static const ULONG_PTR kDfNodeListLink   = 0x100;
	static const ULONG_PTR kDfNodeDevice     = 0x00;
	static const ULONG_PTR kDfNodeOriginal   = 0x98;

	/* 2026-09-19 现机证据（两轮）：①DfDiskLo 钩的是 DRIVER 级 MajorFunction 表，
	 * node+0x00 只是装挂代表设备，exact 设备匹配必漏；②saved-orig 的形态对
	 * storport 系 miniport 是 port（storport.sys）里的框架 dispatch，不在
	 * miniport 自身镜像里 —— 归属判据改为“落在任一已加载驱动镜像内且不在
	 * hooker 镜像内”，唯一候选采纳；exact 设备匹配仍最优先。 */
	PVOID FindSavedSrbHandler(PDEVICE_OBJECT pdo, PLDR_DATA_TABLE_ENTRY64 hooker,
	                          PLDR_DATA_TABLE_ENTRY64 victim)
	{
		PLIST_ENTRY* headPtr = (PLIST_ENTRY*)((UCHAR*)hooker->DllBase + kDfHookListGlobal);
		__try
		{
			if (!MmIsAddressValid(headPtr))
			{
				LogWarn("[DfWalk] headPtr probe failed: %wZ base=%p headPtr=%p",
					&hooker->BaseDllName, hooker->DllBase, headPtr);
				return NULL;
			}
			PLIST_ENTRY head = (PLIST_ENTRY)*headPtr;
			LogWarn("[DfWalk] %wZ base=%p head(P)=%p", &hooker->BaseDllName, hooker->DllBase, head);
			if (!head || !MmIsAddressValid(head))
			{
				LogWarn("[DfWalk] head(P) invalid: %p", head);
				return NULL;
			}
			LogWarn("[DfWalk] victim %wZ base=%p size=0x%X; hooker %wZ base=%p size=0x%X",
				&victim->FullDllName, victim->DllBase, victim->SizeOfImage,
				&hooker->BaseDllName, hooker->DllBase, hooker->SizeOfImage);
			int count = 0;
			PVOID fallback = NULL;
			PLDR_DATA_TABLE_ENTRY64 fallbackMod = NULL;
			int candidates = 0;
			for (PLIST_ENTRY le = head->Flink; le != head; le = le->Flink)
			{
				if (++count > 32) { LogWarn("[DfWalk] walk aborted: >32 nodes (corrupt list?)"); return NULL; }
				UCHAR* node = (UCHAR*)le - kDfNodeListLink;
				if (!MmIsAddressValid(node)) { LogWarn("[DfWalk] node %d probe failed at %p", count, node); break; }
				PVOID dev = *(PVOID*)(node + kDfNodeDevice);
				PVOID orig = *(PVOID*)(node + kDfNodeOriginal);
				LogWarn("[DfWalk] node %d: device=%p saved-orig=%p", count, dev, orig);
				if ((PDEVICE_OBJECT)dev == pdo)
					return orig;
				if ((ULONG_PTR)orig >= (ULONG_PTR)hooker->DllBase &&
					(ULONG_PTR)orig < (ULONG_PTR)hooker->DllBase + hooker->SizeOfImage)
				{
					LogWarn("[DfWalk]   saved-orig lies inside hooker image -- trampoline, skipped");
					continue;
				}
				PLDR_DATA_TABLE_ENTRY64 mod = FindModuleByAddress(orig, victim);
				if (mod)
				{
					LogWarn("[DfWalk]   saved-orig attributed to loaded module %wZ", &mod->FullDllName);
					if (!fallback) { fallback = orig; fallbackMod = mod; }
					candidates++;
				}
			}
			if (candidates == 1)
			{
				LogWarn("[DfWalk] device %p not in records; adopting saved-orig %p (%wZ) by module attribution (1 candidate)",
					pdo, fallback, &fallbackMod->FullDllName);
				return fallback;
			}
			if (candidates > 1)
			{
				LogWarn("[DfWalk] walked %d node(s); %d module candidates -- ambiguous, refusing", count, candidates);
				return NULL;
			}
			LogWarn("[DfWalk] walked %d node(s); no device == PDO %p, no attributing candidate", count, pdo);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			/* 链表正被改写/布局对不上：没找到就放弃，不动粗 */
			LogWarn("[DfWalk] exception while walking hook list (code 0x%08X)", GetExceptionCode());
		}
		return NULL;
	}

	/* 目标 PDO 定下来之后调：检测其 dispatch 是否被钩、若被钩就从 hooker 的
	 * 挂钩记录里捞出真身，存进 g_BypassSrbHandler。
	 * 任何一步对不上都保持 NULL —— 语义回到"可能没写进 baseline"的旧世界，
	 * 绝不带着错误的指针硬闯。 */
	void ResolveSrbBypass(PDEVICE_OBJECT pdo)
	{
		g_BypassSrbHandler = NULL;
		if (!pdo || !pdo->DriverObject || !pdo->DriverObject->DriverSection)
			return;
		PLDR_DATA_TABLE_ENTRY64 victim =
			(PLDR_DATA_TABLE_ENTRY64)pdo->DriverObject->DriverSection;
		const PVOID cur = pdo->DriverObject->MajorFunction[IRP_MJ_SCSI];
		const BOOLEAN ownImage =
			((ULONG_PTR)cur >= (ULONG_PTR)victim->DllBase) &&
			((ULONG_PTR)cur < (ULONG_PTR)victim->DllBase + victim->SizeOfImage);
		if (ownImage) {
			LogInfo("scsi dispatch of %wZ is clean; no bypass needed", victim->FullDllName);
			return;
		}
		PLDR_DATA_TABLE_ENTRY64 hooker = FindModuleByAddress(cur, victim);
		if (!hooker) {
			LogWarn("scsi dispatch hooked by an untraceable module; bypass NOT enabled");
			return;
		}
		PVOID orig = FindSavedSrbHandler(pdo, hooker, victim);
		if (!orig) {
			LogWarn("no saved-original hook record for PDO %p in %wZ; bypass NOT enabled",
				pdo, &hooker->BaseDllName);
			return;
		}
		/* miniport 的原始 dispatch 可能在 port（storport.sys）镜像里：
		 * 信任条件是“在任一已加载模块里、但不在 hooker 镜像里”，
		 * 而不是死认 victim 镜像。 */
		PLDR_DATA_TABLE_ENTRY64 origMod = FindModuleByAddress(orig, victim);
		const BOOLEAN origTrusted =
			origMod &&
			!((ULONG_PTR)orig >= (ULONG_PTR)hooker->DllBase &&
			  (ULONG_PTR)orig < (ULONG_PTR)hooker->DllBase + hooker->SizeOfImage);
		if (!origTrusted) {
			LogWarn("saved-original candidate %p is not inside a loaded driver image (or is inside hooker); refusing to trust it",
				orig);
			return;
		}
		g_BypassSrbHandler = orig;
		LogWarn("BYPASS ENABLED: SRBs go through saved original %p (recovered from %wZ's hook record)",
			orig, &hooker->BaseDllName);
	}

	BOOLEAN AntiFSDHookCallback(PVOID Buffer, SIZE_T Size)
	{
		PDEVICE_OBJECT device = *((PDEVICE_OBJECT*)Buffer);
		PDRIVER_OBJECT driver = device->DriverObject;
		PLDR_DATA_TABLE_ENTRY64 ldr = (PLDR_DATA_TABLE_ENTRY64)driver->DriverSection;

		/* 还原/冻结类软件（DeepFrz 流派）绕开栈底绕行的一条路：不改设备链，
		 * 直接把端口 PDO 所属驱动的 MajorFunction[IRP_MJ_SCSI] 换成指向自己
		 * 映像的桩函数。IoCallDriver 本质是查这张表，所以栈底写得再对也照样被
		 * 拦。判据很简单 —— 分派函数指针应该落在**自己驱动的映像区间**里。 */
		const PVOID scsiHandler = driver->MajorFunction[IRP_MJ_SCSI];
		const BOOLEAN ownImage =
			((ULONG_PTR)scsiHandler >= (ULONG_PTR)ldr->DllBase) &&
			((ULONG_PTR)scsiHandler < (ULONG_PTR)ldr->DllBase + ldr->SizeOfImage);
		LogInfo("stack device %p: %wZ (%wZ), scsi_dispatch=%p%s\n",
			device, driver->DriverName, ldr->FullDllName, scsiHandler,
			ownImage ? "" : "  << OUTSIDE OWN IMAGE");
		if (!ownImage)
		{
			PLDR_DATA_TABLE_ENTRY64 owner = FindModuleByAddress(scsiHandler, ldr);
			if (owner)
			{
				LogWarn("%wZ 的 IRP_MJ_SCSI 被钩: 分派指针落在 %wZ (base=%p size=0x%X) 里\n",
					driver->DriverName, &owner->FullDllName,
					owner->DllBase, owner->SizeOfImage);
				ScanForSavedOriginal(owner, ldr);
			}
			else
			{
				LogWarn("%wZ 的 IRP_MJ_SCSI 被钩: 分派指针不在任何已加载模块内\n",
					driver->DriverName);
			}
		}
		return TRUE;
	}
}

NTSTATUS GetHardDiskDevice(WCHAR DeviceName[], PFILE_OBJECT *FileObject)
{
	UNICODE_STRING ObjectName;
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK StatusBlock;
	PFILE_OBJECT LocalFileObject;
	HANDLE DeviceHandle;
	NTSTATUS status;

	RtlInitUnicodeString(&ObjectName, DeviceName);

	InitializeObjectAttributes(&ObjectAttributes, &ObjectName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

	status = ZwOpenFile(&DeviceHandle, GENERIC_READ, &ObjectAttributes, &StatusBlock,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (NT_SUCCESS(status))
	{
		status = ObReferenceObjectByHandle(DeviceHandle, GENERIC_READ, *IoFileObjectType, KernelMode, (PVOID *)&LocalFileObject, NULL);
		if (NT_SUCCESS(status))
		{
			*FileObject = LocalFileObject;
		}
		ZwClose(DeviceHandle);
	}
	return status;
}

NTSTATUS GetDiskMiniport(WCHAR DeviceName[], PDATA_LIST_ENTRY DeviceObjects)
{
	PDEVICE_OBJECT LowerDevice, LowestDevice;
	PFILE_OBJECT FileObject;
	NTSTATUS status;
	BOOLEAN Found = FALSE;

	status = GetHardDiskDevice(DeviceName, &FileObject);
	if (status == STATUS_SUCCESS)
	{
		LogInfo("Getting lowest device for %ws\n", DeviceName);
		LowerDevice = IoGetLowerDeviceObject(FileObject->DeviceObject);
		while (LowerDevice)
		{
			Found = TRUE;
			LogInfo("Found lower device: %ws\n", LowerDevice->DriverObject->DriverName.Buffer);
			List_AddToTail(DeviceObjects, &LowerDevice, sizeof(LowerDevice));
			LowerDevice = IoGetLowerDeviceObject(LowerDevice);
		}
		if (Found)
		{
			LowestDevice = *((PDEVICE_OBJECT*)(List_GetTailItem(DeviceObjects)->Buffer));
			if (LowestDevice->DriverObject->DriverName.Buffer)
				LogInfo("Found lowest device (Disk Miniport): %ws\n", LowestDevice->DriverObject->DriverName.Buffer);
		}
		else
		{
			status = STATUS_NOT_FOUND;
		}

		ObDereferenceObject(FileObject);
	}
	return status;
}

NTSTATUS SetTargetDisk(WCHAR DiskName[])
{
	if (!IsListEmpty(&TargetDisks.DataList))
		List_DeleteAll(&TargetDisks);
	TargetDisk = NULL;
	NTSTATUS status = GetDiskMiniport(DiskName, &TargetDisks);
	if (NT_SUCCESS(status))
	{
		TargetDisk = *((PDEVICE_OBJECT*)(List_GetTailItem(&TargetDisks)->Buffer));
	}
	List_Visit(&TargetDisks, FSDAntiHook::AntiFSDHookCallback);
	/* 在审计之后再判定旁路：hooker 是谁、saved-original 在哪，都由上面的日志
	 * 先留个底；这一步决定 SRB 是照常走（可能被钩）还是直调真身。 */
	/* 2026-09-19 实验：DfDiskLo 完整逆向证实其 sector 账本只影子 MBR/GPT，
	 * 数据区经 stub 直通 storport —— 故 hive 写入本不需要 bypass。
	 * 本分支强制不走 bypass（g_BypassSrbHandler 恒 NULL，全 IoCallDriver），
	 * 用于在 VM 上实测 (via dispatch) 是否真的落盘，以裁决 bypass 的必要性。 */
	// FSDAntiHook::ResolveSrbBypass(TargetDisk);
	return status;
}

NTSTATUS DeviceApi(PDEVICE_OBJECT Device, PIRP pIrp)
{
	pIrp->IoStatus.Status = STATUS_SUCCESS;
	pIrp->IoStatus.Information = 0;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS DeviceIoctl(PDEVICE_OBJECT Device, PIRP pIrp)
{
	NTSTATUS status;
	PIO_STACK_LOCATION StackLocation = IoGetCurrentIrpStackLocation(pIrp);
	PVOID SystemBuffer = pIrp->AssociatedIrp.SystemBuffer;
	ULONG InBufferLength = StackLocation->Parameters.DeviceIoControl.InputBufferLength;
	ULONG OutBufferLength = StackLocation->Parameters.DeviceIoControl.OutputBufferLength;
	ULONG ControlCode = StackLocation->Parameters.DeviceIoControl.IoControlCode;
	ULONG info = 0;
	PWCHAR Buffer;

	switch (ControlCode)
	{
	case CTL_CHANGE_TARGET_DISK:
		Buffer = (PWCHAR)ExAllocatePool(NonPagedPool, InBufferLength + 1);
		RtlZeroMemory(Buffer, InBufferLength + 1);
		RtlCopyMemory(Buffer, SystemBuffer, InBufferLength);
		Buffer[InBufferLength / 2] = '\0';
		status = SetTargetDisk(Buffer);
		if (NT_SUCCESS(status)) LogInfo("Changed target disk to %ls\n", Buffer);
		else LogWarn("Failed to change target disk to %ls, error code: 0x%.8X\n", Buffer, status);
		break;
	case CTL_UNLOCK_FILE:
		Buffer = (PWCHAR)ExAllocatePool(NonPagedPool, InBufferLength + 1);
		RtlZeroMemory(Buffer, InBufferLength + 1);
		RtlCopyMemory(Buffer, SystemBuffer, InBufferLength);
		Buffer[InBufferLength / 2] = '\0';
		status = UnlockFile(Buffer);
		if (NT_SUCCESS(status)) LogInfo("Unlocked file %ls\n", Buffer);
		else LogWarn("Failed to unlock file %ls, error code: 0x%.8X\n", Buffer, status);
		break;
	default:
		LogWarn("Unknown CODE!\n");
		status = STATUS_UNSUCCESSFUL;
		break;
	}

	pIrp->IoStatus.Status = status;
	pIrp->IoStatus.Information = info;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return status;
}

NTSTATUS DeviceCreate(
	IN PDEVICE_OBJECT pDeviceObject,
	IN PIRP pIrp
)
{
	pIrp->IoStatus.Information = 0;
	pIrp->IoStatus.Status = SetTargetDisk(L"\\Device\\Harddisk0\\DR0");

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}

NTSTATUS DeviceClose(
	IN PDEVICE_OBJECT pDeviceObject,
	IN PIRP pIrp
)
{
	pIrp->IoStatus.Information = 0;
	pIrp->IoStatus.Status = STATUS_SUCCESS;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}

NTSTATUS DeviceWrite(
	IN PDEVICE_OBJECT pDeviceObject,
	IN PIRP pIrp
) {
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(pIrp);
	PVOID input = pIrp->AssociatedIrp.SystemBuffer;
	ULONG length = stack->Parameters.Write.Length;
	LONGLONG byteOffset = stack->Parameters.Write.ByteOffset.QuadPart;
	ULONG sector = (ULONG)(byteOffset / g_ulBytesPerSector);
	ULONG inSector = (ULONG)(byteOffset % g_ulBytesPerSector);
	ULONG sectors = (inSector + length + g_ulBytesPerSector - 1) /
		g_ulBytesPerSector;
	NTSTATUS status = STATUS_SUCCESS;
	PVOID rmwBuffer = NULL;
	PVOID verifyBuffer = NULL;

	LogInfo("Write request: offset=%lld sector=%lu in_sector=%lu sectors=%lu length=%lu\n",
		byteOffset, sector, inSector, sectors, length);
	if (length == 0) {
		pIrp->IoStatus.Information = 0;
		pIrp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(pIrp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;
	}

	/* 当前上层通常提交扇区对齐的请求。对齐请求直接写，不先读再写整扇区；
	 * 只有非对齐请求才使用 RMW。这样不会用一次旧的扇区读取覆盖另一次更新。 */
	if (inSector != 0 || (length % g_ulBytesPerSector) != 0) {
		rmwBuffer = ExAllocatePool(NonPagedPool, sectors * g_ulBytesPerSector);
		if (!rmwBuffer) status = STATUS_INSUFFICIENT_RESOURCES;
		if (NT_SUCCESS(status)) {
			status = ScsiReadDisk(TargetDisk, sector, rmwBuffer, sectors);
			if (NT_SUCCESS(status)) {
				RtlCopyMemory((PUCHAR)rmwBuffer + inSector, input, length);
				status = ScsiWriteDisk(TargetDisk, sector, rmwBuffer, sectors);
			}
		}
	} else {
		status = ScsiWriteDisk(TargetDisk, sector, input, sectors);
	}

	if (NT_SUCCESS(status)) {
		verifyBuffer = ExAllocatePool(NonPagedPool, sectors * g_ulBytesPerSector);
		if (!verifyBuffer) status = STATUS_INSUFFICIENT_RESOURCES;
		else {
			status = ScsiReadDisk(TargetDisk, sector, verifyBuffer, sectors);
			if (NT_SUCCESS(status)) {
				const PUCHAR expected = rmwBuffer ? (PUCHAR)rmwBuffer + inSector : (PUCHAR)input;
				const PUCHAR actual = (PUCHAR)verifyBuffer + inSector;
				ULONG i;
				for (i = 0; i < length && expected[i] == actual[i]; ++i) {}
				if (i != length) {
					LogWarn("Write verification failed: byte=%lu length=%lu disk_offset=%lld expected=0x%.2X actual=0x%.2X\n",
						i, length, byteOffset + i, expected[i], actual[i]);
					status = STATUS_DEVICE_DATA_ERROR;
				}
			}
		}
	}
	if (verifyBuffer) ExFreePool(verifyBuffer);
	if (rmwBuffer) ExFreePool(rmwBuffer);
	pIrp->IoStatus.Information = NT_SUCCESS(status) ? length : 0;
	pIrp->IoStatus.Status = status;
	if (!NT_SUCCESS(status)) LogWarn("Write failed: status=0x%.8X\n", status);
	else LogInfo("Write completed: offset=%lld length=%lu\n", byteOffset, length);
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return status;
}
NTSTATUS DeviceRead(
	IN PDEVICE_OBJECT pDeviceObject,
	IN PIRP pIrp
) {
	PIO_STACK_LOCATION StackLocation = IoGetCurrentIrpStackLocation(pIrp);
	PVOID SystemBuffer = pIrp->AssociatedIrp.SystemBuffer;
	ULONG OutBufferLength = StackLocation->Parameters.Read.Length;
	IO_STATUS_BLOCK StatusBlock = { 0 };

	LONGLONG OldOffset = StackLocation->Parameters.Read.ByteOffset.QuadPart;
	ULONG Offset = OldOffset / g_ulBytesPerSector;
	/* 与 DeviceWrite 同理：扇区数要覆盖含起始偏移的整段，否则不对齐跨界读
	 * 会读出 Buffer 末尾（把非分页池邻接数据当文件内容交给调用方）。 */
	ULONG NewOffset = (ULONG)(OldOffset - Offset * g_ulBytesPerSector);
	ULONG Sectors = (NewOffset + OutBufferLength + g_ulBytesPerSector - 1) / g_ulBytesPerSector;

	LogInfo("Read disk request: Offset=%lld, OffsetSector=%lu, Sectors=%lu, Size=%lu\n", OldOffset, Offset, Sectors, OutBufferLength);

	NTSTATUS status = STATUS_SUCCESS;
	PVOID Buffer = (PVOID)ExAllocatePool(NonPagedPool, Sectors * g_ulBytesPerSector);
	if (!Buffer)
		status = STATUS_INSUFFICIENT_RESOURCES;

	if (NT_SUCCESS(status))
	{
		status = ScsiReadDisk(TargetDisk, Offset, Buffer, Sectors);
		if (NT_SUCCESS(status))
		{
			RtlCopyMemory(SystemBuffer, (*(PUCHAR*)&Buffer) + NewOffset, OutBufferLength);
		}
	}

	pIrp->IoStatus.Information = NT_SUCCESS(status) ? OutBufferLength : 0;
	pIrp->IoStatus.Status = status;

	if (!NT_SUCCESS(status)) LogWarn("Read disk failed, error code: 0x%.8X\n", status);
	else LogInfo("Read disk success!\n");

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
	DriverObject->DriverUnload = &DriverUnload;
	NTSTATUS status;

	for (int i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++)
	{
		DriverObject->MajorFunction[i] = DeviceApi;
	}
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceIoctl;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DeviceCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DeviceClose;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = DeviceWrite;
	DriverObject->MajorFunction[IRP_MJ_READ] = DeviceRead;

	RtlInitUnicodeString(&DeviceName, DEVICE_NAME);
	status = IoCreateDevice(DriverObject, 0, &DeviceName, FILE_DEVICE_UNKNOWN, 0, NULL, &pDevice);
	if (!NT_SUCCESS(status))
	{
		LogErr("Create Device Faild!\n");
		return STATUS_UNSUCCESSFUL;
	}

	RtlInitUnicodeString(&SymLinkName, SYMBOLIC_LINK_NAME);
	status = IoCreateSymbolicLink(&SymLinkName, &DeviceName);
	if (!NT_SUCCESS(status))
	{
		LogErr("Create SymLink Faild!\n");
		IoDeleteDevice(pDevice);
		return STATUS_UNSUCCESSFUL;
	}

	LogInfo("Initialize Success\n");

	pDevice->Flags = DO_BUFFERED_IO;

	InitializeListHead(&TargetDisks.DataList);

	return STATUS_SUCCESS;
}

void DriverUnload(IN PDRIVER_OBJECT DriverObject)
{
	IoDeleteSymbolicLink(&SymLinkName);
	IoDeleteDevice(pDevice);

	LogInfo("Driver Unloaded!\n");
}