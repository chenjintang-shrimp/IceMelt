#include "ScsiDisk.h"
#include "DataList.h"
#include "FileUnlock.h"

/* 这套 Win7 WDK 头没有导出的 MmIsAddressValid 原型（NT 导出，老驱动通吃）。
 * 手动声明；语义：虚址当前可安全解引用（映射可用）返回 TRUE。 */
extern "C" NTKERNELAPI BOOLEAN NTAPI MmIsAddressValid(_In_ PVOID VirtualAddress);

// CTL_REBOOT_SYSTEM 直接触发 bugcheck，而不是走 HalReturnToFirmware(HalRebootRoutine)。
//
// 两条路都会立刻复位机器，区别在于 bugcheck 之后内核不再有机会把内存里脏的注册表
// hive 页刷回磁盘 —— SecMelt 在调用前刚用裸盘把导出的 SYSTEM hive 写上去，任何"温和"
// 的关机/重启路径都可能用内存里的旧 hive 覆盖它。bugcheck 会先写崩溃转储（直写扇区的
// 崩溃转储栈，落到 pagefile），再按系统设置自动重启。
//
// 0x0D000721 是自定义代码：Windows 里 0xDxxxxxxx 段留给第三方，不会与内置代码冲突。
#define SECMELT_BUGCHECK_CODE   ((ULONG)0x0D000721)

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath);
VOID DriverUnload(IN PDRIVER_OBJECT DriverObject);

PDEVICE_OBJECT pDevice;
UNICODE_STRING DeviceName;
UNICODE_STRING SymLinkName;

DATA_LIST_ENTRY TargetDisks = { 0 };
PDEVICE_OBJECT TargetDisk = NULL;

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
	case CTL_REBOOT_SYSTEM:
		// 不返回：内核在此停住，不刷脏页、不做关机通知
		KeBugCheckEx(SECMELT_BUGCHECK_CODE,
			(ULONG_PTR)Device,
			(ULONG_PTR)ControlCode,
			0,
			0);
		LogWarn("KeBugCheckEx returned, which must never happen!\n");
		status = STATUS_UNSUCCESSFUL;
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