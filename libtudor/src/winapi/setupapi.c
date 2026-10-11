#include <stddef.h>
#include <libusb.h>
#include "internal.h"

//Minimal SetupDi*/CreateFile support for exactly one purpose: letting a driver that talks to its
//device via the real WinUsb.dll API (see winusb.c) discover a device path and CreateFile it,
//which is the normal prerequisite for WinUsb_Initialize on real Windows. This project's SetupDi
//imports were previously entirely unresolved (would abort loudly if called - see loader.c/stub.c),
//and live tracing during this investigation showed the driver never reaches them, meaning it gets
//stuck before ever calling WinUsb_Initialize at all.
//
//Real SetupDi enumerates by device interface class GUID against the system's actual device
//tree - something this project has no equivalent of. Since this bridge only ever has exactly one
//real device to offer for the whole process lifetime (see winusb.c's g_active_usb_dev), the
//ClassGuid/Enumerator/ everything else is ignored and a single fixed fake device path is always
//returned as the (and only) enumerated interface; CreateFileA recognizes that exact path and hands
//back a handle wrapping the one active libusb device, exactly like a real WinUsb device file
//handle would.

#define FAKE_DEVICE_PATH "\\\\?\\TUDOR#USB#000#{f72fe0d4-cbcb-407d-8814-9ed673d0dd6b}"

//Also accept FAKE_WDF_DEVICE_NAME (com_activate.c's "\Device\usbpdo-0", which OnPrepareHardware
//CreateFileA/W's directly) so that call succeeds too. A previous pass rejected this, reasoning
//(from the driver's INF `UmdfDispatcher=WinUsb` directive alone) that the real USB access model
//must be IWDFDevice::GetDefaultIoTarget()/raw-IOCTL, not WinUsb_Initialize, and treated the SIGSEGV
//this exposes past `WinUsb_Initialize: claimed interface 0, 5 pipes` as evidence of a wrong path.
//Reconsidered after external research (see FINDINGS.md): a working reference port of a sibling
//Synaptics Tudor/WBDI-family driver only reaches real pairing/TLS/hardware communication via
//exactly this kind of real WinUsb-backed bulk transfer - "untested path -> new crash" has been the
//correct-direction pattern for every other real bug found this session (WideCharToMultiByte,
//RetrieveDeviceName, HeapFree, ctx->field_0x100, ...), not evidence of a dead end by itself. Worth
//pursuing the resulting crash as a genuine bug rather than reverting on sight.
static bool is_recognized_fake_device_path(const char *name) {
    return name && (strcmp(name, FAKE_DEVICE_PATH) == 0 || strcmp(name, "\\Device\\usbpdo-0") == 0);
}

extern libusb_device_handle *tudor_get_active_usb_device(void);

typedef struct {
    DWORD cbSize;
    GUID ClassGuid;
    DWORD DevInst;
    ULONG_PTR Reserved;
} SP_DEVINFO_DATA;

typedef struct {
    DWORD cbSize;
    GUID InterfaceClassGuid;
    DWORD Flags;
    ULONG_PTR Reserved;
} SP_DEVICE_INTERFACE_DATA;

typedef struct {
    DWORD cbSize;
    char DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_A;

typedef struct {
    DWORD cbSize;
    char16_t DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_W;

#define INVALID_HANDLE_VALUE_RAW ((HANDLE) (uintptr_t) -1)
//Any nonzero, non -1 value works as our fake HDEVINFO - never dereferenced.
#define FAKE_DEVINFO_SET ((HANDLE) (uintptr_t) 0x7364656C /* 'sdel' */)

__winfnc HANDLE SetupDiGetClassDevsA(const GUID *ClassGuid, const char *Enumerator, HANDLE hwndParent, DWORD Flags) {
    log_debug("SetupDiGetClassDevsA(Enumerator='%s', Flags=0x%x) -> fake device info set", Enumerator ? Enumerator : "(null)", Flags);
    return FAKE_DEVINFO_SET;
}
WINAPI(SetupDiGetClassDevsA)

__winfnc HANDLE SetupDiGetClassDevsW(const GUID *ClassGuid, const char16_t *Enumerator, HANDLE hwndParent, DWORD Flags) {
    log_debug("SetupDiGetClassDevsW called -> fake device info set");
    return FAKE_DEVINFO_SET;
}
WINAPI(SetupDiGetClassDevsW)

__winfnc BOOL SetupDiEnumDeviceInterfaces(HANDLE DeviceInfoSet, SP_DEVINFO_DATA *DeviceInfoData, const GUID *InterfaceClassGuid, DWORD MemberIndex, SP_DEVICE_INTERFACE_DATA *DeviceInterfaceData) {
    if(MemberIndex > 0) {
        winerr_set_code(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if(DeviceInterfaceData) {
        DeviceInterfaceData->InterfaceClassGuid = InterfaceClassGuid ? *InterfaceClassGuid : (GUID) {0};
        DeviceInterfaceData->Flags = 1; //SPINT_ACTIVE
        DeviceInterfaceData->Reserved = 0;
    }
    log_debug("SetupDiEnumDeviceInterfaces(index=0) -> our one fake device");
    return TRUE;
}
WINAPI(SetupDiEnumDeviceInterfaces)

__winfnc BOOL SetupDiGetDeviceInterfaceDetailA(HANDLE DeviceInfoSet, SP_DEVICE_INTERFACE_DATA *DeviceInterfaceData, SP_DEVICE_INTERFACE_DETAIL_DATA_A *DeviceInterfaceDetailData, DWORD DeviceInterfaceDetailDataSize, DWORD *RequiredSize, SP_DEVINFO_DATA *DeviceInfoData) {
    size_t path_len = sizeof(FAKE_DEVICE_PATH);
    size_t needed = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + path_len;
    if(RequiredSize) *RequiredSize = (DWORD) needed;

    if(!DeviceInterfaceDetailData || DeviceInterfaceDetailDataSize < needed) {
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(DeviceInterfaceDetailData->DevicePath, FAKE_DEVICE_PATH, path_len);
    if(DeviceInfoData) *DeviceInfoData = (SP_DEVINFO_DATA) { .cbSize = sizeof(SP_DEVINFO_DATA) };
    log_debug("SetupDiGetDeviceInterfaceDetailA -> '%s'", FAKE_DEVICE_PATH);
    return TRUE;
}
WINAPI(SetupDiGetDeviceInterfaceDetailA)

__winfnc BOOL SetupDiGetDeviceInterfaceDetailW(HANDLE DeviceInfoSet, SP_DEVICE_INTERFACE_DATA *DeviceInterfaceData, SP_DEVICE_INTERFACE_DETAIL_DATA_W *DeviceInterfaceDetailData, DWORD DeviceInterfaceDetailDataSize, DWORD *RequiredSize, SP_DEVINFO_DATA *DeviceInfoData) {
    char16_t *wpath = winstr_from_str(FAKE_DEVICE_PATH);
    size_t path_len = (winstr_len(wpath) + 1) * sizeof(char16_t);
    size_t needed = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + path_len;
    if(RequiredSize) *RequiredSize = (DWORD) needed;

    if(!DeviceInterfaceDetailData || DeviceInterfaceDetailDataSize < needed) {
        free(wpath);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(DeviceInterfaceDetailData->DevicePath, wpath, path_len);
    free(wpath);
    if(DeviceInfoData) *DeviceInfoData = (SP_DEVINFO_DATA) { .cbSize = sizeof(SP_DEVINFO_DATA) };
    log_debug("SetupDiGetDeviceInterfaceDetailW -> '%s'", FAKE_DEVICE_PATH);
    return TRUE;
}
WINAPI(SetupDiGetDeviceInterfaceDetailW)

__winfnc BOOL SetupDiDestroyDeviceInfoList(HANDLE DeviceInfoSet) {
    return TRUE;
}
WINAPI(SetupDiDestroyDeviceInfoList)

__winfnc BOOL SetupDiEnumDeviceInfo(HANDLE DeviceInfoSet, DWORD MemberIndex, SP_DEVINFO_DATA *DeviceInfoData) {
    if(MemberIndex > 0) {
        winerr_set_code(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if(DeviceInfoData) *DeviceInfoData = (SP_DEVINFO_DATA) { .cbSize = sizeof(SP_DEVINFO_DATA) };
    return TRUE;
}
WINAPI(SetupDiEnumDeviceInfo)

//CreateFileA/W: unresolved before this (would abort loudly if called - see loader.c). Only
//meaningfully implemented for the fake device paths recognized above (the SetupDi-enumerated
//FAKE_DEVICE_PATH, and separately FAKE_RETRIEVED_DEVICE_NAME which the driver reaches directly via
//its own IWDFDevice::RetrieveDeviceName result, not through SetupDi at all); anything else fails
//cleanly rather than crashing, in case some other code path calls this for something we don't
//otherwise support.
__winfnc HANDLE CreateFileA(const char *name, DWORD access, DWORD share_mode, void *sec_attrs, DWORD creation_disp, DWORD flags_attrs, HANDLE template_file) {
    log_debug("CreateFileA('%s')", name ? name : "(null)");
    if(!is_recognized_fake_device_path(name)) {
        winerr_set_code(ERROR_MOD_NOT_FOUND);
        return INVALID_HANDLE_VALUE_RAW;
    }

    libusb_device_handle *usb_dev = tudor_get_active_usb_device();
    if(!usb_dev) {
        winerr_set_code(ERROR_GEN_FAILURE);
        return INVALID_HANDLE_VALUE_RAW;
    }

    //This project's winusb.c ignores the handle value entirely and always operates on the one
    //active libusb device (see its own comment for why), but this must still be a REAL winhandle:
    //the driver CloseHandle()s it in OnReleaseHardware, and returning the raw libusb device pointer
    //here made winhandle_destroy free/corrupt libusb's own device handle (libusb_close/libusb_exit
    //then aborted on a corrupted mutex). No destructor - the libusb device is owned by our caller.
    HANDLE handle = winhandle_create(usb_dev, NULL);
    log_debug("CreateFileA: opened fake WinUsb device path, handle=%p", (void*) handle);
    return handle;
}
WINAPI(CreateFileA)

__winfnc HANDLE CreateFileW(const char16_t *wname, DWORD access, DWORD share_mode, void *sec_attrs, DWORD creation_disp, DWORD flags_attrs, HANDLE template_file) {
    char *name = winstr_to_str(wname);
    log_debug("CreateFileW('%s')", name ? name : "(null)");
    bool matches = is_recognized_fake_device_path(name);
    free(name);

    if(!matches) {
        winerr_set_code(ERROR_MOD_NOT_FOUND);
        return INVALID_HANDLE_VALUE_RAW;
    }

    libusb_device_handle *usb_dev = tudor_get_active_usb_device();
    if(!usb_dev) {
        winerr_set_code(ERROR_GEN_FAILURE);
        return INVALID_HANDLE_VALUE_RAW;
    }
    HANDLE handle = winhandle_create(usb_dev, NULL); //See CreateFileA
    log_debug("CreateFileW: opened fake WinUsb device path, handle=%p", (void*) handle);
    return handle;
}
WINAPI(CreateFileW)
