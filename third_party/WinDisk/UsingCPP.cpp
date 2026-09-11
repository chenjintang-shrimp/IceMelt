#include "UsingCPP.hpp"

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

/// A pool tag for this module

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// An alternative implementation of a C++ exception handler. Issues a bug check.
DECLSPEC_NORETURN
VOID
KernelStlpRaiseException(
	_In_ ULONG bug_check_code
)
{
	KdBreakPoint();
#pragma warning(push)
#pragma warning(disable : 28159)
	KeBugCheck(bug_check_code);
#pragma warning(pop)
}

DECLSPEC_NORETURN
VOID
__cdecl
_invalid_parameter_noinfo_noreturn(
	VOID
)
{
	KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
}

namespace std {
	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xbad_alloc(
			VOID
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xinvalid_argument(
			_In_z_ CHAR CONST*
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xlength_error(
			_In_z_ CHAR CONST*
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xout_of_range(
			_In_z_ CHAR CONST*
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xoverflow_error(
			_In_z_ CHAR CONST*
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xruntime_error(
			_In_z_ CHAR CONST*
		)
	{
		KernelStlpRaiseException(KMODE_EXCEPTION_NOT_HANDLED);
	}
}  // namespace std

   // An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
__cdecl
operator new(
	_In_ size_t size
	)
{
	if (size == 0) {
		size = 1;
	}

	CONST PVOID p = ExAllocatePoolWithTag(NonPagedPool, size, kKstlpPoolTag);
	if (!p)
		KernelStlpRaiseException(MUST_SUCCEED_POOL_EMPTY);
	return p;
}

// An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete(
	_In_ PVOID p
	)
{
	if (p)
		ExFreePoolWithTag(p, kKstlpPoolTag);
}

// An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete(
	_In_ PVOID p, _In_ size_t size
	)
{
	UNREFERENCED_PARAMETER(size);
	if (p)
		ExFreePoolWithTag(p, kKstlpPoolTag);
}

// overload new[] and delete[] operator
_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
__cdecl
operator new[](
	_In_ size_t size
	)
{
	if (size == 0)
		size = 1;

	CONST PVOID p = ExAllocatePoolWithTag(NonPagedPool, size, kKstlpPoolTag);
	if (!p)
		KernelStlpRaiseException(MUST_SUCCEED_POOL_EMPTY);
	return p;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete[](
	_In_ PVOID p
	)
{
	if (p)
		ExFreePoolWithTag(p, kKstlpPoolTag);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete[](
	_In_ PVOID p, _In_ size_t size
	)
{
	UNREFERENCED_PARAMETER(size);
	if (p)
		ExFreePoolWithTag(p, kKstlpPoolTag);
}

//_CRTIMP2_PURE_IMPORT _Prhand std::_Raise_handler;

EXTERN_C
VOID
__cdecl
_invoke_watson(
	WCHAR CONST* CONST expression,
	WCHAR CONST* CONST function_name,
	WCHAR CONST* CONST file_name,
	UINT CONST line_number,
	uintptr_t CONST reserved
)
{
	UNREFERENCED_PARAMETER(expression);
	UNREFERENCED_PARAMETER(function_name);
	UNREFERENCED_PARAMETER(file_name);
	UNREFERENCED_PARAMETER(line_number);
	UNREFERENCED_PARAMETER(reserved);
}

ATEXIT_ENTRY::ATEXIT_ENTRY(
	__in VOID(__cdecl* destructor)(VOID),
	__in ATEXIT_ENTRY* next
)
{
	Destructor = destructor;
	Next = next;
}

ATEXIT_ENTRY::~ATEXIT_ENTRY()
{
	Destructor();
}

EXTERN_C
INT
__cdecl atexit(
	__in VOID(__cdecl* destructor)(VOID)
)
{
	if (!destructor)
		return 0;

	ATEXIT_ENTRY* entry = new ATEXIT_ENTRY(destructor, g_pTopAtexitEntry);
	if (!entry)
		return 0;
	g_pTopAtexitEntry = entry;
	return 1;
}

#if defined(_IA64_) || defined(_AMD64_)
#pragma section(".CRT$XCA",long,read)
__declspec(allocate(".CRT$XCA")) VOID(*__ctors_begin__[1])(VOID) = { 0 };
#pragma section(".CRT$XCZ",long,read)
__declspec(allocate(".CRT$XCZ")) VOID(*__ctors_end__[1])(VOID) = { 0 };
#pragma data_seg()
#else
#pragma data_seg(".CRT$XCA")
VOID(*__ctors_begin__[1])(VOID) = { 0 };
#pragma data_seg(".CRT$XCZ")
VOID(*__ctors_end__[1])(VOID) = { 0 };
#pragma data_seg()
#endif

#pragma data_seg(".STL$A")
VOID(*___StlStartInitCalls__[1])(VOID) = { 0 };
#pragma data_seg(".STL$L")
VOID(*___StlEndInitCalls__[1])(VOID) = { 0 };
#pragma data_seg(".STL$M")
VOID(*___StlStartTerminateCalls__[1])(VOID) = { 0 };
#pragma data_seg(".STL$Z")
VOID(*___StlEndTerminateCalls__[1])(VOID) = { 0 };
#pragma data_seg()

EXTERN_C
VOID
__cdecl doexit(
	__in INT /*code*/,
	__in INT /*quick*/,
	__in INT /*retcaller*/
)
{
	for (ATEXIT_ENTRY* entry = g_pTopAtexitEntry; entry;)
	{
		ATEXIT_ENTRY* next = entry->Next;
		delete entry;
		entry = next;
	}
}

EXTERN_C
INT
__cdecl _cinit(
	__in INT
)
{
	for (void(**ctor)(void) = __ctors_begin__ + 1;
		ctor < __ctors_end__;
		ctor++)
	{
		(*ctor)();
	}
	return 0;
}