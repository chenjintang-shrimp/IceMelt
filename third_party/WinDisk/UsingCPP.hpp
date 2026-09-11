#pragma once
#ifndef _STL_H_
#define _STL_H_
#include <ntifs.h>
#include <windef.h>
//#include <memory>
using namespace std;

#undef _HAS_EXCEPTIONS

// This enables use of STL in kernel-mode.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#define _HAS_EXCEPTIONS 0
#pragma clang diagnostic pop

// See common.h for details
#pragma prefast(disable : 30030)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

/// A pool tag for this module
static const ULONG kKstlpPoolTag = 'WKDV';

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
);

DECLSPEC_NORETURN
VOID
__cdecl
_invalid_parameter_noinfo_noreturn(
	VOID
);

namespace std {
	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xbad_alloc(
			VOID
		);

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xinvalid_argument(
			_In_z_ CHAR CONST*
		);

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xlength_error(
			_In_z_ CHAR CONST*
		);

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xout_of_range(
			_In_z_ CHAR CONST*
		);

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xoverflow_error(
			_In_z_ CHAR CONST*
		);

	DECLSPEC_NORETURN
		VOID
		__cdecl
		_Xruntime_error(
			_In_z_ CHAR CONST*
		);
}  // namespace std

   // An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
__cdecl
operator new(
	_In_ size_t size
	);

// An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete(
	_In_ PVOID p
	);

// An alternative implementation of the new operator
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete(
	_In_ PVOID p, _In_ size_t size
	);

// overload new[] and delete[] operator
_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
__cdecl
operator new[](
	_In_ size_t size
	);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete[](
	_In_ PVOID p
	);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
__cdecl
operator delete[](
	_In_ PVOID p, _In_ size_t size
	);

//extern _CRTIMP2_PURE_IMPORT _Prhand std::_Raise_handler;

EXTERN_C
VOID
__cdecl
_invoke_watson(
	WCHAR CONST* CONST expression,
	WCHAR CONST* CONST function_name,
	WCHAR CONST* CONST file_name,
	UINT CONST line_number,
	uintptr_t CONST reserved
);

struct ATEXIT_ENTRY
{
	ATEXIT_ENTRY(
		__in VOID(__cdecl* destructor)(VOID),
		__in ATEXIT_ENTRY* next
	);

	~ATEXIT_ENTRY();

	VOID(_cdecl* Destructor)();
	ATEXIT_ENTRY* Next;
};

static ATEXIT_ENTRY* g_pTopAtexitEntry = NULL;

EXTERN_C
INT
__cdecl atexit(
	__in VOID(__cdecl* destructor)(VOID)
);

#if defined(_IA64_) || defined(_AMD64_)
#pragma section(".CRT$XCA",long,read)
extern __declspec(allocate(".CRT$XCA")) VOID(*__ctors_begin__[1])(VOID);
#pragma section(".CRT$XCZ",long,read)
extern __declspec(allocate(".CRT$XCZ")) VOID(*__ctors_end__[1])(VOID);
#pragma data_seg()
#else
#pragma data_seg(".CRT$XCA")
extern VOID(*__ctors_begin__[1])(VOID);
#pragma data_seg(".CRT$XCZ")
extern VOID(*__ctors_end__[1])(VOID);
#pragma data_seg()
#endif

#pragma data_seg(".STL$A")
extern VOID(*___StlStartInitCalls__[1])(VOID);
#pragma data_seg(".STL$L")
extern VOID(*___StlEndInitCalls__[1])(VOID);
#pragma data_seg(".STL$M")
extern VOID(*___StlStartTerminateCalls__[1])(VOID);
#pragma data_seg(".STL$Z")
extern VOID(*___StlEndTerminateCalls__[1])(VOID);
#pragma data_seg()

EXTERN_C
VOID
__cdecl doexit(
	__in INT /*code*/,
	__in INT /*quick*/,
	__in INT /*retcaller*/
);

EXTERN_C
INT
__cdecl _cinit(
	__in INT
);

#pragma clang diagnostic pop
#endif