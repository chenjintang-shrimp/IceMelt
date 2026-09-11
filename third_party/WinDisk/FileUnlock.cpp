#include "Pch.h"
#include "FileUnlock.h"

#define KERNEL_HANDLE_MASK ((ULONG_PTR)((LONG)0x80000000))

NTSTATUS ForceCloseHandle(HANDLE pid, HANDLE handle)
{
	PEPROCESS			eprocess;
	KAPC_STATE			apcstate;
	NTSTATUS			status;
	MODE				mode = UserMode;
	OBJECT_HANDLE_FLAG_INFORMATION objectinfo;

	status = PsLookupProcessByProcessId(pid, &eprocess);
	if (eprocess == NULL || !MmIsAddressValid(eprocess))
		return status;

	__try
	{
		KeStackAttachProcess(eprocess, &apcstate);
		if (PsGetCurrentProcess() == PsInitialSystemProcess)
		{
			handle = (HANDLE)((ULONG_PTR)handle | KERNEL_HANDLE_MASK);
			mode = KernelMode;
		}
		objectinfo.Inherit = 0;
		objectinfo.ProtectFromClose = 0;
		status = ObSetHandleAttributes(handle, &objectinfo, mode);
		if (NT_SUCCESS(status)) {
			status = ZwClose(handle);
		}
		KeUnstackDetachProcess(&apcstate);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = STATUS_UNSUCCESSFUL;
	}
	ObDereferenceObject(eprocess);
	return status;
}

typedef struct _OBJECT_TYPE_INFORMATION {
	UNICODE_STRING          TypeName;
	ULONG                   TotalNumberOfHandles;
	ULONG                   TotalNumberOfObjects;
	WCHAR                   Unused1[8];
	ULONG                   HighWaterNumberOfHandles;
	ULONG                   HighWaterNumberOfObjects;
	WCHAR                   Unused2[8];
	ACCESS_MASK             InvalidAttributes;
	GENERIC_MAPPING         GenericMapping;
	ACCESS_MASK             ValidAttributes;
	BOOLEAN                 SecurityRequired;
	BOOLEAN                 MaintainHandleCount;
	USHORT                  MaintainTypeList;
	POOL_TYPE               PoolType;
	ULONG                   DefaultPagedPoolCharge;
	ULONG                   DefaultNonPagedPoolCharge;
} OBJECT_TYPE_INFORMATION, *POBJECT_TYPE_INFORMATION;

ULONG ObjectTypeIndexByName(PWCHAR object_type_name)
{
	ULONG		index = -1;
	NTSTATUS	status;
	ULONG		bufsize = PAGE_SIZE;
	PVOID		buf = ExAllocatePoolWithTag(NonPagedPool, bufsize, 'obte');
	while ((status = ZwQueryObject(
		NULL,
		(OBJECT_INFORMATION_CLASS)3, //ObjectTypesInformation,
		buf,
		bufsize,
		NULL
	)) == STATUS_INFO_LENGTH_MISMATCH) {
		ExFreePoolWithTag(buf, 'obte');
		bufsize *= 2;
		buf = ExAllocatePoolWithTag(NonPagedPool, bufsize, 'obte');
	}
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(buf, 'obte');
		return index;
	}
	ULONG number_types = *(ULONG *)buf;
	POBJECT_TYPE_INFORMATION obj_info = (POBJECT_TYPE_INFORMATION)(((PUCHAR)buf) + ALIGN_UP(sizeof(number_types), ULONG_PTR));
	for (ULONG i = 0; i < number_types; i++) {
		UNICODE_STRING t_type_name;
		RtlInitUnicodeString(&t_type_name, object_type_name);

		if (0 == RtlCompareUnicodeString(&t_type_name, &(obj_info->TypeName), TRUE)) {
			index = i + 2;
			break;
		}
		obj_info = (POBJECT_TYPE_INFORMATION)
			((PCHAR)(obj_info + 1) + ALIGN_UP(obj_info->TypeName.MaximumLength, ULONG_PTR));
	}
	if (buf) {
		ExFreePoolWithTag(buf, 'obte');
	}
	return index;
}

NTSTATUS UnlockFile(PWCHAR FileName)
{
	PWCHAR		path = FileName;

	NTSTATUS	status = STATUS_UNSUCCESSFUL;
	ULONG		size = 0x10000;
	PVOID		buffer = NULL;
	UINT64		handlecount = 0;
	PSYSTEM_HANDLE_TABLE_ENTRY_INFO tableinfo = NULL;
	ULONG		count = 0;
	UCHAR		filetypeindex = 28;  // set file object type index

	filetypeindex = (UCHAR)ObjectTypeIndexByName(L"File");
	if (filetypeindex == -1)
		return status;

	buffer = ExAllocatePoolWithTag(NonPagedPool, size, 'enhd');
	if (buffer == NULL)
		return STATUS_MEMORY_NOT_ALLOCATED;

	RtlZeroMemory(buffer, size);
	status = ZwQuerySystemInformation(SystemHandleInformation, buffer, size, &size);
	while (status == STATUS_INFO_LENGTH_MISMATCH)
	{
		ExFreePoolWithTag(buffer, 'enhd');
		buffer = ExAllocatePoolWithTag(NonPagedPool, size, 'enhd');
		if (buffer == NULL)
			return STATUS_MEMORY_NOT_ALLOCATED;
		RtlZeroMemory(buffer, size);
		status = ZwQuerySystemInformation(SystemHandleInformation, buffer, size, &size);
	}

	if (!NT_SUCCESS(status))
		return status;

	handlecount = (UINT64)(((SYSTEM_HANDLE_INFORMATION *)buffer)->NumberOfHandles);
	tableinfo = (SYSTEM_HANDLE_TABLE_ENTRY_INFO *)((SYSTEM_HANDLE_INFORMATION *)buffer)->Handles;

	for (int i = 0; i < handlecount; i++)
	{
		USHORT			processid = tableinfo[i].UniqueProcessId;
		HANDLE			handle = (HANDLE)tableinfo[i].HandleValue;
		ULONG			typeindex = (ULONG)tableinfo[i].ObjectTypeIndex;
		LPVOID			object = tableinfo[i].Object;

		CLIENT_ID					cid = { 0 };
		OBJECT_ATTRIBUTES			oa = { 0 };
		HANDLE						hprocess = NULL;
		HANDLE						hdupobj = NULL;
		OBJECT_BASIC_INFORMATION	basicinfo = { 0 };
		POBJECT_NAME_INFORMATION	nameinfo = NULL;
		POBJECT_TYPE_INFORMATION    typeinfo = NULL;
		ULONG						refcount = 0;
		ULONG						flag = 0;

		if (typeindex != filetypeindex)
			continue;

		cid.UniqueProcess = (HANDLE)processid;
		cid.UniqueThread = (HANDLE)0;
		InitializeObjectAttributes(&oa, NULL, 0, NULL, NULL);

		while (1) {
			status = ZwOpenProcess(&hprocess, PROCESS_DUP_HANDLE, &oa, &cid);
			if (!NT_SUCCESS(status))
			{
				LogWarn("ZwOpenProcess Error! ErrorCode: %d\n", status);
				break;
			}
			status = ZwDuplicateObject(hprocess, handle, NtCurrentProcess(), &hdupobj, PROCESS_ALL_ACCESS, 0, DUPLICATE_SAME_ACCESS);
			if (!NT_SUCCESS(status))
			{
				LogWarn("ZwDuplicateObject Error! ErrorCode: %d\n", status);
				break;
			}
			nameinfo = (POBJECT_NAME_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, 1024, 'enhd');
			if (nameinfo == NULL)
			{
				status = STATUS_MEMORY_NOT_ALLOCATED;
				break;
			}
			RtlZeroMemory(nameinfo, 1024);
			status = ZwQueryObject(hdupobj, (OBJECT_INFORMATION_CLASS)1, nameinfo, 1024, &flag); //ObjectNameInformation

			if (nameinfo->Name.Length > 0) {
				WCHAR pathlower[260] = { 0 };
				WCHAR namelower[260] = { 0 };

				wcsncpy(pathlower, path, wcslen(path));
				wcsncpy(namelower, nameinfo->Name.Buffer, nameinfo->Name.Length / sizeof(WCHAR));

				_wcslwr(pathlower);
				_wcslwr(namelower);
				if (wcsstr(namelower, pathlower)) {
					// filter the file path
					ZwQueryObject(hdupobj, ObjectBasicInformation, &basicinfo, sizeof(OBJECT_BASIC_INFORMATION), NULL);
					typeinfo = (POBJECT_TYPE_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, 256, 'enhd');
					if (typeinfo == NULL) {
						status = STATUS_MEMORY_NOT_ALLOCATED;
						break;
					}
					RtlZeroMemory(typeinfo, 256);
					status = ZwQueryObject(hdupobj, (OBJECT_INFORMATION_CLASS)2, typeinfo, 256, &flag); // ObjectTypeInformation
					refcount = basicinfo.ReferenceCount - basicinfo.HandleCount; // maybe bug?
					/*item->pid = (HANDLE)processid;
					item->handle = handle;
					item->object = object;
					item->ref_count = refcount;
					item->type_index = typeindex;
					RtlCopyMemory(item->name, nameinfo->Name.Buffer, sizeof(WCHAR) * nameinfo->Name.Length);
					RtlCopyMemory(item->type_name, typeinfo->TypeName.Buffer, sizeof(WCHAR) * typeinfo->TypeName.Length);*/
					ForceCloseHandle((HANDLE)processid, handle);
					count++;
					LogInfo("NAME:%wZ TYPE:%wZ\n", &(nameinfo->Name), &(typeinfo->TypeName));
				}
			}
			break;
		}

		if (nameinfo) ExFreePoolWithTag(nameinfo, 'enhd');
		if (typeinfo) ExFreePoolWithTag(typeinfo, 'enhd');
		if (hdupobj) ZwClose(hdupobj);
		if (hprocess) ZwClose(hprocess);
	}

	if (buffer)  ExFreePoolWithTag(buffer, 'enhd');
	return status;
}
