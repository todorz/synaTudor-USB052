#include <asm/prctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "internal.h"

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress;
    void *lpMaximumApplicationAddress;
    ULONG_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO;

typedef struct {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} OSVERSIONINFOW;

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXA;

__winfnc NTSTATUS RtlGetVersion(OSVERSIONINFOW *ver) {
    ver->dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
    ver->dwMajorVersion = 10;
    ver->dwMinorVersion = 0;
    ver->dwBuildNumber = 17134;
    ver->dwPlatformId = 2; //VER_PLATFORM_WIN32_NT
    return 0;
}
WINAPI(RtlGetVersion)

__winfnc ULONGLONG VerSetConditionMask(ULONGLONG cond_mask, DWORD type_mask, BYTE condition) {
    return cond_mask;
}
WINAPI(VerSetConditionMask)

__winfnc void GetSystemInfo(SYSTEM_INFO *info) {
    info->wProcessorArchitecture = 9; //PROCESSOR_ARCHITECTURE_AMD64
    info->dwPageSize = 4096;
    info->lpMinimumApplicationAddress = (void*) 0;
    info->lpMaximumApplicationAddress = (void*) UINTPTR_MAX;
    info->dwActiveProcessorMask = 0b1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 8664; //PROCESSOR_AMD_X8664
    info->dwAllocationGranularity = 8;
    info->wProcessorLevel = 0;
    info->wProcessorRevision = 0;
}
WINAPI(GetSystemInfo)

__winfnc BOOL VerifyVersionInfoW(ULONGLONG condition_mask, DWORD type_mask, ULONGLONG cond_mask) {
    //TODO
    return TRUE;
}
WINAPI(VerifyVersionInfoW)

__winfnc BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const char16_t *str, DWORD rev, void *descrpt, ULONG *descrpt_size) {
    //TODO
    if(descrpt_size) *descrpt_size = 0;
    return TRUE;
}
WINAPI(ConvertStringSecurityDescriptorToSecurityDescriptorW)

__winfnc void *EncodePointer(void *ptr) {
    return ptr;
}
WINAPI(EncodePointer)

__winfnc void *DecodePointer(void *ptr) {
    return ptr;
}
WINAPI(DecodePointer)

//Real Windows returns 0 (and sets last error) when the requested firmware table provider/ID isn't
//available; drivers using this for optional OEM-specific hardware detection (e.g. SMBIOS/ACPI
//quirks) are expected to handle that by falling back to default behavior.
__winfnc UINT GetSystemFirmwareTable(DWORD provider_sig, DWORD table_id, void *buf, DWORD buf_size) {
    return 0;
}
WINAPI(GetSystemFirmwareTable)

//Power-state/processor-performance queries; not needed for fingerprint capture, and real Windows
//returns this same code for information levels it doesn't support on a given system.
__winfnc NTSTATUS CallNtPowerInformation(DWORD info_level, void *in_buf, ULONG in_len, void *out_buf, ULONG out_len) {
    return 0xC00000BB; /* STATUS_NOT_SUPPORTED */
}
WINAPI(CallNtPowerInformation)

typedef void __winfnc OOBE_COMPLETED_CALLBACK(void *ctx, BOOLEAN timer_or_wait_fired);

//There's no real OOBE (Windows first-run setup) concept in this environment, so it's always
//already "complete" - the callback is invoked immediately rather than left pending forever.
__winfnc BOOL RegisterWaitUntilOOBECompleted(OOBE_COMPLETED_CALLBACK *callback, void *ctx, void **out_wait_handle) {
    callback(ctx, FALSE);
    *out_wait_handle = (void*) 1;
    return TRUE;
}
WINAPI(RegisterWaitUntilOOBECompleted)

//No real Windows special-folder layout exists here; any writable scratch directory is good
//enough for a driver just looking for somewhere to cache/store its own data, regardless of which
//CSIDL_* folder it thinks it's asking for.
__winfnc HRESULT SHGetFolderPathA(HANDLE hwnd, int csidl, HANDLE token, DWORD flags, char *out_path) {
    strcpy(out_path, "/tmp");
    return 0; /* S_OK */
}
WINAPI(SHGetFolderPathA)

//Uses '/' rather than '\' as the separator, consistent with the plain POSIX-style paths this
//environment hands out elsewhere (e.g. SHGetFolderPathA above) instead of real Windows paths.
__winfnc BOOL CreateDirectoryA(const char *path, void *sec_attrs) {
    if(mkdir(path, 0755) == 0) return TRUE;

    if(errno == EEXIST) winerr_set_code(0xB7); /* ERROR_ALREADY_EXISTS */
    else winerr_set_errno();
    return FALSE;
}
WINAPI(CreateDirectoryA)

__winfnc BOOL PathFileExistsA(const char *path) {
    return access(path, F_OK) == 0;
}
WINAPI(PathFileExistsA)

__winfnc BOOL PathAppendA(char *path, const char *more) {
    if(*more == '/' || *more == '\\') more++;

    size_t path_len = strlen(path);
    if(path_len > 0 && path[path_len-1] != '/' && path[path_len-1] != '\\') path[path_len++] = '/';

    strcpy(path + path_len, more);
    return TRUE;
}
WINAPI(PathAppendA)