#pragma once
#include <windef.h>

#define DEVICE_NAME L"\\Device\\WDDevice"
#define SYMBOLIC_LINK_NAME L"\\??\\WDLink"

#ifdef CTL_CODE
#undef CTL_CODE
#endif
#define CTL_CODE( DeviceType, Function, Method, Access ) (                 \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)
#define METHOD_BUFFERED                 0
#define FILE_DEVICE_UNKNOWN             0x00000022
#define MAKECODE(Function) (CTL_CODE(FILE_DEVICE_UNKNOWN,Function,METHOD_BUFFERED,FILE_ANY_ACCESS))

#define CTL_DRIVER_VERIFICATION MAKECODE(0x1C00)
#define CTL_CHANGE_TARGET_DISK MAKECODE(0x1C01)
#define CTL_UNLOCK_FILE MAKECODE(0x1C02)

static const BYTE AuthorizationContext[] = "2265EBC24AA8192537B24166AAFB07396E8D39B94B8A270A02D316751181867824AEAC5A83B8CEF7A0911EB486FA00EA199AEB37D55F5961F3D01158CA3D497C";
