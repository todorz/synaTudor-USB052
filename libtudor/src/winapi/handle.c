#include "internal.h"

HANDLE winhandle_create(void *data, winhandle_destr_fnc *destr) {
    HANDLE handle = (HANDLE) malloc(sizeof(struct _HANDLE));
    if(!handle) {
        perror("Couldn't allocate memory for handle");
        abort();
    }

    handle->data = data;
    handle->destr = destr;
    return handle;
}

void winhandle_destroy(HANDLE handle) {
    //NULL is never a valid allocated handle (same reasoning as CloseHandle's -1 pseudo-handle
    //check): tolerate it as a no-op rather than crashing, since callers passing NULL/"no handle
    //set yet" through cleanup paths is a common, otherwise-harmless pattern.
    if(!handle) return;

    if(handle->destr) handle->destr(handle->data);
    free(handle);
}

#define STD_INPUT_HANDLE ((DWORD) -10)
#define STD_OUTPUT_HANDLE ((DWORD) -11)
#define STD_ERROR_HANDLE ((DWORD) -12)

__winfnc HANDLE GetStdHandle(DWORD type) {
    switch(type) {
        case STD_INPUT_HANDLE: return NULL;
        case STD_OUTPUT_HANDLE: return NULL;
        case STD_ERROR_HANDLE: return NULL;
        default: return INVALID_HANDLE_VALUE;
    }
}
WINAPI(GetStdHandle)

__winfnc BOOL CloseHandle(HANDLE handle) {
    //Real Windows documents CloseHandle on the GetCurrentProcess() pseudo-handle (-1) as a no-op
    //that still returns success (there's nothing to actually free); treat it that way here too,
    //rather than dereferencing -1 as if it were a real allocated winhandle struct.
    if(handle == (HANDLE) -1) return TRUE;

    winhandle_destroy(handle);
    return TRUE;
}
WINAPI(CloseHandle)