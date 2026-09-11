#include "ScsiDisk.h"
#include "DataList.h"
#include "FileUnlock.h"

typedef enum _FIRMWARE_REENTRY {
	HalHaltRoutine,
	HalPowerDownRoutine,
	HalRestartRoutine,
	HalRebootRoutine,
	HalInteractiveModeRoutine,
	HalMaximumRoutine
} FIRMWARE_REENTRY, *PFIRMWARE_REENTRY;

EXTERN_C NTKERNELAPI VOID NTAPI HalReturnToFirmware(
	LONG lReturnType
);

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

	BOOLEAN AntiFSDHookCallback(PVOID Buffer, SIZE_T Size)
	{
		PDEVICE_OBJECT pDevObj = *((PDEVICE_OBJECT*)Buffer);
		PDRIVER_OBJECT pDrvObj = pDevObj->DriverObject;
		LogInfo("Disk DriverName: %wZ, DriverPath: %wZ\n", pDrvObj->DriverName, ((PLDR_DATA_TABLE_ENTRY64)pDrvObj->DriverSection)->FullDllName);
		NTSTATUS stat = STATUS_UNSUCCESSFUL;
		if (!NT_SUCCESS(stat))
		{
			LogWarn("Restore original SCSI failed! ErrorCode: 0x%.8X\n", stat);
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
		HalReturnToFirmware(HalRebootRoutine);
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
	PIO_STACK_LOCATION StackLocation = IoGetCurrentIrpStackLocation(pIrp);
	PVOID SystemBuffer = pIrp->AssociatedIrp.SystemBuffer;
	ULONG InBufferLength = StackLocation->Parameters.Write.Length;
	IO_STATUS_BLOCK StatusBlock = { 0 };

	LONGLONG OldOffset = StackLocation->Parameters.Write.ByteOffset.QuadPart;
	ULONG Offset = OldOffset / g_ulBytesPerSector;
	ULONG Sectors = InBufferLength / g_ulBytesPerSector + (InBufferLength % g_ulBytesPerSector == 0 ? 0 : 1);

	LogInfo("Write disk request: Offset=%lld, OffsetSector=%lu, Sectors=%lu, Size=%lu\n", OldOffset, Offset, Sectors, InBufferLength);

	NTSTATUS status = STATUS_SUCCESS;
	PVOID Buffer = (PVOID)ExAllocatePool(NonPagedPool, Sectors * g_ulBytesPerSector);
	if (!Buffer)
		status = STATUS_INSUFFICIENT_RESOURCES;

	if (NT_SUCCESS(status))
	{
		status = ScsiReadDisk(TargetDisk, Offset, Buffer, Sectors);
		if (NT_SUCCESS(status))
		{
			ULONG NewOffset = OldOffset - Offset * g_ulBytesPerSector;
			RtlCopyMemory((*(PUCHAR*)&Buffer) + NewOffset, SystemBuffer, InBufferLength);
			status = ScsiWriteDisk(&TargetDisks, Offset, Buffer, Sectors);
		}
	}

	pIrp->IoStatus.Information = InBufferLength;
	pIrp->IoStatus.Status = status;
	
	if (!NT_SUCCESS(status)) LogWarn("Write disk failed, error code: 0x%.8X\n", status);
	else LogInfo("Write disk success!\n");

	if (Buffer)
		ExFreePool(Buffer);

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
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
	ULONG Sectors = OutBufferLength / g_ulBytesPerSector + (OutBufferLength % g_ulBytesPerSector == 0 ? 0 : 1);

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
			ULONG NewOffset = OldOffset - Offset * g_ulBytesPerSector;
			RtlCopyMemory(SystemBuffer, (*(PUCHAR*)&Buffer) + NewOffset, OutBufferLength);
		}
	}

	pIrp->IoStatus.Information = OutBufferLength;
	pIrp->IoStatus.Status = status;

	if (!NT_SUCCESS(status)) LogWarn("Read disk failed, error code: 0x%.8X\n", status);
	else LogInfo("Read disk success!\n");

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
	//DriverObject->DriverUnload = &DriverUnload;

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