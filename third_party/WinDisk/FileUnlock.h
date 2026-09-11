#pragma once

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
	USHORT	UniqueProcessId;
	USHORT	CreatorBackTraceIndex;
	UCHAR	ObjectTypeIndex;
	UCHAR	HandleAttributes;
	USHORT	HandleValue;
	PVOID	Object;
	ULONG	GrantedAccess;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO;

typedef struct _SYSTEM_HANDLE_INFORMATION {
	HANDLE NumberOfHandles;
	SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
} SYSTEM_HANDLE_INFORMATION, *PSYSTEM_HANDLE_INFORMATION;

typedef struct _OBJECT_BASIC_INFORMATION {
	ULONG                   Attributes;
	ACCESS_MASK             DesiredAccess;
	ULONG                   HandleCount;
	ULONG                   ReferenceCount;
	ULONG                   PagedPoolUsage;
	ULONG                   NonPagedPoolUsage;
	ULONG                   Reserved[3];
	ULONG                   NameInformationLength;
	ULONG                   TypeInformationLength;
	ULONG                   SecurityDescriptorLength;
	LARGE_INTEGER           CreationTime;
} OBJECT_BASIC_INFORMATION, *POBJECT_BASIC_INFORMATION;

typedef struct _OBJECT_HANDLE_FLAG_INFORMATION {
	BOOLEAN Inherit;
	BOOLEAN ProtectFromClose;
} OBJECT_HANDLE_FLAG_INFORMATION, *POBJECT_HANDLE_FLAG_INFORMATION;

typedef enum _SYSTEM_INFORMATION_CLASS {
	SystemBasicInformation,
	SystemProcessorInformation,
	SystemPerformanceInformation,
	SystemTimeOfDayInformation,
	SystemNotImplemented1,
	SystemProcessesAndThreadsInformation,
	SystemCallCounts,
	SystemConfigurationInformation,
	SystemProcessorTimes,
	SystemGlobalFlag,
	SystemNotImplemented2,
	SystemModuleInformation,
	SystemLockInformation,
	SystemNotImplemented3,
	SystemNotImplemented4,
	SystemNotImplemented5,
	SystemHandleInformation,
	SystemObjectInformation,
	SystemPagefileInformation,
	SystemInstructionEmulationCounts,
	SystemInvalidInfoClass1,
	SystemCacheInformation,
	SystemPoolTagInformation,
	SystemProcessorStatistics,
	SystemDpcInformation,
	SystemNotImplemented6,
	SystemLoadImage,
	SystemUnloadImage,
	SystemTimeAdjustment,
	SystemNotImplemented7,
	SystemNotImplemented8,
	SystemNotImplemented9,
	SystemCrashDumpInformation,
	SystemExceptionInformation,
	SystemCrashDumpStateInformation,
	SystemKernelDebuggerInformation,
	SystemContextSwitchInformation,
	SystemRegistryQuotaInformation,
	SystemLoadAndCallImage,
	SystemPrioritySeparation,
	SystemNotImplemented10,
	SystemNotImplemented11,
	SystemInvalidInfoClass2,
	SystemInvalidInfoClass3,
	SystemTimeZoneInformation,
	SystemLookasideInformation,
	SystemSetTimeSlipEvent,
	SystemCreateSession,
	SystemDeleteSession,
	SystemInvalidInfoClass4,
	SystemRangeStartInformation,
	SystemVerifierInformation,
	SystemAddVerifier,
	SystemSessionProcessesInformation
} SYSTEM_INFORMATION_CLASS;

EXTERN_C NTSTATUS NTAPI  ObSetHandleAttributes(HANDLE Handle, POBJECT_HANDLE_FLAG_INFORMATION HandleFlags, KPROCESSOR_MODE PreviousMode);

EXTERN_C NTSTATUS NTAPI
ZwQuerySystemInformation(
	__in SYSTEM_INFORMATION_CLASS SystemInformationClass,
	__out_bcount_opt(SystemInformationLength) PVOID SystemInformation,
	__in ULONG SystemInformationLength,
	__out_opt PULONG ReturnLength
);

NTSTATUS ForceCloseHandle(HANDLE pid, HANDLE handle);

NTSTATUS UnlockFile(PWCHAR FileName);
