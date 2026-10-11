#include <pthread.h>
#include "internal.h"

static pthread_rwlock_t modules_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct winmodule *modules_head;

static void module_destr(struct winmodule *module) {
    if(module->cmdline) return;

    //Free the module
    module->handle = NULL;
    winmodule_unregister(module);
    free((void*) module->name);
    free(module);
}

struct winmodule *winmodule_find(const char *name) {
    cant_fail_ret(pthread_rwlock_rdlock(&modules_lock));

    struct winmodule *module = NULL;
    for(struct winmodule *m = modules_head; m != NULL; m = m->next) {
        if(strcmp(m->name, name) == 0) {
            module = m;
            break;
        }
    }

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
    return module;
}

void winmodule_register(struct winmodule *module) {
    cant_fail_ret(pthread_rwlock_wrlock(&modules_lock));

    module->handle = winhandle_create(module, (winhandle_destr_fnc*) module_destr);

    module->prev = NULL;
    module->next = modules_head;
    if(modules_head) modules_head->prev = module;
    modules_head = module;

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
}

void winmodule_unregister(struct winmodule *module) {
    cant_fail_ret(pthread_rwlock_wrlock(&modules_lock));

    if(module->handle) {
        winhandle_destroy(module->handle);
        module->handle = NULL;
    }

    if(module->prev) module->prev->next = module->next;
    else modules_head = module->next;
    if(module->next) module->next->prev = module->prev;

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
}

static __thread struct winmodule *cur_module;

struct winmodule *winmodule_get_cur() {
    WIN_CLOBBER_NONVOL_REGS
    return cur_module;
}

void winmodule_set_cur(struct winmodule *module) {
    cur_module = module;
}

__winfnc HANDLE LoadLibraryExW(const char16_t *name, HANDLE file, DWORD flags) {
    //We don't support loading librarys dynamically, but some stdlib functions have to be loaded that way
    //As such return dummy modules
    struct winmodule *module = (struct winmodule*) malloc(sizeof(struct winmodule));
    if(!module) { winerr_set_errno(); return NULL; }
    *module = (struct winmodule) {0};
    module->name = winstr_to_str(name);
    log_debug("DIAG: LoadLibraryExW('%s') -> dummy module (no real content loaded)", module->name);
    winmodule_register(module);
    return module->handle;
}
WINAPI(LoadLibraryExW)

//Same "dummy module" approach as LoadLibraryExW above - we don't support loading real library
//content dynamically, but some callers just need a handle back for a name they'll pass to
//GetProcAddress or check for null-ness, not actually execute code from.
__winfnc HANDLE LoadLibraryA(const char *name) {
    struct winmodule *module = (struct winmodule*) malloc(sizeof(struct winmodule));
    if(!module) { winerr_set_errno(); return NULL; }
    *module = (struct winmodule) {0};
    module->name = strdup(name);
    log_debug("DIAG: LoadLibraryA('%s') -> dummy module (no real content loaded)", module->name);
    winmodule_register(module);
    return module->handle;
}
WINAPI(LoadLibraryA)

__winfnc BOOL FreeLibrary(HANDLE handle) {
    winhandle_destroy(handle);
    return TRUE;
}
WINAPI(FreeLibrary)

__winfnc HANDLE GetModuleHandleA(const char *name) {
    //A NULL name is a legitimate, common request for "the currently executing module's own
    //handle" (real Win32 GetModuleHandle semantics), not an error.
    if(!name) {
        struct winmodule *cur = winmodule_get_cur();
        return cur ? cur->handle : NULL;
    }

    struct winmodule *module = (struct winmodule*) winmodule_find(name);
    if(!module) log_warn("GetModuleHandleA: Module '%s' not found!", name);
    return module ? module->handle : NULL;
}
WINAPI(GetModuleHandleA)

__winfnc HANDLE GetModuleHandleW(const char16_t *name) {
    if(!name) {
        struct winmodule *cur = winmodule_get_cur();
        return cur ? cur->handle : NULL;
    }

    char *cname = winstr_to_str(name);
    struct winmodule *module = (struct winmodule*) winmodule_find(cname);
    if(!module) log_warn("GetModuleHandleW: Module '%s' not found!", cname);
    free(cname);
    return module ? module->handle : NULL;
}
WINAPI(GetModuleHandleW)

#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
__winfnc BOOL GetModuleHandleExW(DWORD flags, const char16_t *name, HANDLE *out) {
    if(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        log_warn("GetModuleHandleExW called with unsupported flag GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS! [addr=%p]", name);
        return FALSE;
    }

    if(!name) {
        struct winmodule *cur = winmodule_get_cur();
        if(!cur) return FALSE;
        *out = cur->handle;
        return TRUE;
    }

    char *cname = winstr_to_str(name);
    struct winmodule *module = (struct winmodule*) winmodule_find(cname);
    free(cname);

    if(module) *out = module->handle;
    return module != NULL;
}
WINAPI(GetModuleHandleExW)

//Real GetModuleFileName always returns a full, absolute path - callers routinely strip the
//filename back off to get at the containing directory (e.g. to look for a companion config file
//alongside the module). module->name here is just the bare DLL name ("synaBscAdapter52.dll", no
//path), which made any such strip-to-directory operation legitimately return an empty string -
//confirmed live as the reason synaBscAdapter52.dll's own StartCapture path treats its engine
//context as never-initialized (a length-0 check on the stripped directory gates a flag the driver
//needs set to complete a real capture). Synthesizing a plausible absolute path here (only when
//the name doesn't already look like one) fixes that class of caller without touching module->name
//itself, which winmodule_find and friends still match against by the bare name.
static char *module_fake_full_path(const char *name) {
    char *path;
    if(strchr(name, '\\') || strchr(name, '/')) path = strdup(name);
    else cant_fail(asprintf(&path, "C:\\Windows\\System32\\drivers\\UMDF\\%s", name));
    return path;
}

__winfnc DWORD GetModuleFileNameA(HANDLE handle, char *name, DWORD size) {
    struct winmodule *module = handle ? (struct winmodule*) handle->data : cur_module;

    char *full_path = module_fake_full_path(module->name);
    int name_len = strlen(full_path);

    if(size == 0) {
        free(full_path);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else if(name_len+1 <= size) {
        memcpy(name, full_path, name_len+1);
        winerr_clear();
        free(full_path);
        return name_len;
    } else {
        name_len = size - 1;
        memcpy(name, full_path, name_len+1);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        free(full_path);
        return name_len;
    }
}
WINAPI(GetModuleFileNameA)

__winfnc DWORD GetModuleFileNameW(HANDLE handle, char16_t *name, DWORD size) {
    struct winmodule *module = handle ? (struct winmodule*) handle->data : cur_module;

    char *full_path = module_fake_full_path(module->name);
    char16_t *wname = winstr_from_str(full_path);
    free(full_path);
    int wname_len = winstr_len(wname);

    if(size == 0) {
        free(wname);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else if(wname_len+1 <= size / sizeof(char16_t)) {
        memcpy(name, wname, (wname_len+1) * sizeof(char16_t));
        winerr_clear();
        free(wname);
        return wname_len;
    } else {
        wname_len = size / sizeof(char16_t) - 1;
        memcpy(name, wname, (wname_len+1) * sizeof(char16_t));
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        free(wname);
        return wname_len;
    }
}
WINAPI(GetModuleFileNameW)

__winfnc BOOL DisableThreadLibraryCalls(HANDLE handle) {
    return TRUE;
}
WINAPI(DisableThreadLibraryCalls)

__winfnc void *GetProcAddress(HANDLE handle, const char *name) {
    if(!handle) {
        log_warn("GetProcAddress: Called with NULL module handle! [name '%s']", name);
        winerr_set_code(ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    struct winmodule *module = (struct winmodule*) handle->data;

    //Check if it's an ordinal import
    uintptr_t nbits = (uintptr_t) name;
    if(!(nbits & ~((128ull << sizeof(uintptr_t)) - 1))) {
        int ord = (int) (nbits & ((128ull << sizeof(uintptr_t)) - 1));
        log_warn("GetProcAddress: Attempted ordinal import! module '%s' [%p] ord %d", module->name, module, ord);
        return NULL;
    }

    //Try to resolve the Windows API function
    void *resolv = resolve_windows_api(name);
    log_debug("DIAG: GetProcAddress('%s') from module '%s' -> %p", name, module->name, resolv);
    if(!resolv) winerr_set();
    return resolv;
}
WINAPI(GetProcAddress)