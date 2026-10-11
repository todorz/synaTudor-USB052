#include <asm/prctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include "internal.h"

static struct {
    ULONG alloc_size;
    ULONG size;
    ULONG flags;
    ULONG debug_flags;
    //...
} process_params;

static struct {
    BOOLEAN inherited_addr_space, read_img_file_exec_opts, being_debugged, __pad1;
    HANDLE mutant;
    void *image_base_addr;
    void *peb_ldr_data;
    void *proc_params;
    void *sub_sys_data;
    HANDLE proc_heap;
    //...
} process_peb;

static __thread struct {
    void *cur_seh_frame;
    void *stack_base, *stack_limit;
    void *sub_system_tib, *fiber_data;
    void *arbitrary_data;
    void *teb_address;

    void *env_ptr;
    uintptr_t proc_id, thread_id;
    void *active_rpc;
    void *thread_local_storage;
    void *peb_ptr;
    //...
} thread_tib;

__constr static void init_peb() {
    //TODO This is incredibly hacky and crude

    //Initialize the process parameters
    process_params.alloc_size = 0;
    process_params.size = 0;
    process_params.flags = 0;
    process_params.debug_flags = 0;

    //Initialize the Process Environment Block
    process_peb.inherited_addr_space = FALSE;
    process_peb.read_img_file_exec_opts = TRUE;
    process_peb.being_debugged = FALSE;
    //As with the TIB fields above, NULL is used for these rarely-read fields instead of a fake
    //non-null placeholder, since code that checks "if (field)" before dereferencing fails
    //gracefully against NULL but crashes against a bogus non-null pointer.
    process_peb.mutant = NULL;
    process_peb.image_base_addr = NULL;
    process_peb.peb_ldr_data = NULL;
    process_peb.proc_params = &process_params;
    process_peb.sub_sys_data = NULL;
    process_peb.proc_heap = (HANDLE) -1;
}

__constr void win_init_tib() {
    //TODO This is incredibly hacky and crude

    //Initialize the Thread Information Block
    //cur_seh_frame (NT_TIB.ExceptionList) is a legacy x86 SEH-chain vestige; real x64 code uses
    //table-based SEH via the PE's unwind info instead and doesn't walk this field, so it's set to
    //the same "unused" sentinel real Windows uses rather than a fake placeholder.
    thread_tib.cur_seh_frame = (void*) -1;

    //stack_base/stack_limit (NT_TIB.StackBase/StackLimit, at %gs:0x8/%gs:0x10) are read by real
    //driver code such as the CRT's _chkstk stack-probing loop (used for dynamically-sized stack
    //allocations), which touches memory based on StackLimit to trigger guard-page growth. Leaving
    //these as placeholder values risks that code touching arbitrary unrelated memory instead of
    //harmlessly no-op'ing, so use this thread's real stack bounds instead.
    pthread_attr_t stack_attr;
    void *real_stack_addr; size_t real_stack_size;
    if(
        pthread_getattr_np(pthread_self(), &stack_attr) == 0 &&
        pthread_attr_getstack(&stack_attr, &real_stack_addr, &real_stack_size) == 0
    ) {
        thread_tib.stack_limit = real_stack_addr;
        thread_tib.stack_base = (uint8_t*) real_stack_addr + real_stack_size;
        pthread_attr_destroy(&stack_attr);
    } else {
        //pthread_getattr_np can still fail here (e.g. inside a sandbox that blocks reading
        ///proc/self/maps, which it needs for the main thread specifically). A real, valid, in-
        //bounds stack address - even an imprecise one - is far safer than 0xdeadc0de: it keeps
        //_chkstk's guard-page-touching loop inside real, mapped stack memory instead of touching
        //arbitrary unrelated memory. 8MiB is the common default Linux stack size (see RLIMIT_STACK).
        void *approx_stack_ptr = &stack_attr;
        thread_tib.stack_limit = approx_stack_ptr - 8*1024*1024;
        thread_tib.stack_base = approx_stack_ptr + 64*1024;
    }

    //These remaining fields are rarely read by ordinary driver code, but a fake non-null sentinel
    //like 0xdeadc0de is strictly worse than NULL: code that defensively checks "if (field) use it"
    //will skip a NULL gracefully, but will happily dereference a non-null garbage pointer and
    //crash (as observed - a driver thread dereferenced one of these directly). NULL at least fails
    //the same way real Windows would for a field that's genuinely unset.
    thread_tib.sub_system_tib = NULL;
    thread_tib.fiber_data = NULL;
    thread_tib.arbitrary_data = NULL;
    //teb_address (NT_TIB.Self) is a real, commonly-checked self-referential pointer (code sometimes
    //validates NtCurrentTeb()->Self == NtCurrentTeb()), so it gets a real, valid answer.
    thread_tib.teb_address = &thread_tib;

    thread_tib.env_ptr = NULL;
    thread_tib.proc_id = getpid();
    thread_tib.thread_id = win_get_thread_id();
    thread_tib.active_rpc = NULL;
    thread_tib.thread_local_storage = NULL;
    thread_tib.peb_ptr = &process_peb;
    syscall(SYS_arch_prctl, ARCH_SET_GS, &thread_tib);
}

DWORD win_get_thread_id() { return (DWORD) syscall(__NR_gettid); }

__winfnc HANDLE GetCurrentProcess() {
    return (HANDLE) -1;
}
WINAPI(GetCurrentProcess)

__winfnc DWORD GetCurrentProcessId() {
    return getpid();
}
WINAPI(GetCurrentProcessId)

//Real multi-process isolation/handle duplication isn't supported here; the common case (a driver
//opening a handle to itself, e.g. to query its own token/memory info) is supported by returning
//the same pseudo-handle GetCurrentProcess does, and opening any other PID fails cleanly rather
//than fabricating a handle to a process we can't actually operate on.
__winfnc HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    if(pid == (DWORD) getpid()) return (HANDLE) -1;
    log_warn("OpenProcess: Opening a process other than the current one isn't supported! [pid %u]", pid);
    winerr_set_code(ERROR_INVALID_PARAMETER);
    return NULL;
}
WINAPI(OpenProcess)

//A fixed placeholder, not a real filesystem path - good enough for a driver that's just querying/
//logging its own image path rather than parsing it as an actual NT path. Deliberately not resolved
//via readlink("/proc/self/exe", ...): under tudor-host's sandbox, readlink is restricted to the
//narrow "/proc/self/fd/<usb_fd>" pattern used to recover the USB device path, and any other path
//(this one included) is treated as a sandbox escape attempt and aborts the process.
__winfnc DWORD K32GetProcessImageFileNameA(HANDLE process, char *out_name, DWORD size) {
    if(process != (HANDLE) -1) {
        log_warn("K32GetProcessImageFileNameA: Only the current process is supported!");
        winerr_set_code(ERROR_INVALID_PARAMETER);
        return 0;
    }

    static const char fake_path[] = "C:\\Windows\\System32\\Drivers\\WudfHost.exe";
    DWORD len = (DWORD) (sizeof(fake_path) - 1);
    if(size > 0 && len >= size) len = size - 1;

    memcpy(out_name, fake_path, len);
    if(size > 0) out_name[len] = 0;
    return len;
}
WINAPI(K32GetProcessImageFileNameA)

__winfnc DWORD GetCurrentThreadId() {
    return win_get_thread_id();
}
WINAPI(GetCurrentThreadId)

__winfnc BOOL TerminateProcess(HANDLE proc, UINT exit_code) {
    if(proc != (HANDLE) -1) return FALSE;
    log_error("TerminateProcess called with exit code 0x%x!", exit_code);
    abort();
}
WINAPI(TerminateProcess)

typedef struct {
    DWORD cb;
    const char16_t *lpReserved;
    const char16_t *lpDesktop;
    const char16_t *lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW;

__winfnc void GetStartupInfoW(STARTUPINFOW *info) {
    static char16_t *desktop = NULL;
    if(!desktop) desktop = winstr_from_str("TUDORHOST");

    info->cb = sizeof(STARTUPINFOW);
    info->lpReserved = NULL;
    info->lpDesktop = desktop;
    info->lpTitle = NULL;
    info->dwX = info->dwY = 0;
    info->dwXSize = info->dwYSize = 0;
    info->dwXCountChars = info->dwYCountChars = 0;
    info->dwFillAttribute = 0;
    info->dwFlags = 0;
    info->wShowWindow = 0;
    info->cbReserved2 = 0;
    info->lpReserved2 = NULL;
    info->hStdInput = info->hStdOutput = info->hStdError = NULL;
}
WINAPI(GetStartupInfoW);

__winfnc const char *GetCommandLineA() {
    return winmodule_get_cur()->cmdline;
}
WINAPI(GetCommandLineA)

__winfnc const char16_t *GetCommandLineW() {
    static char16_t *cmd_line;
    if(!cmd_line) cmd_line = winstr_from_str(GetCommandLineA());
    return cmd_line;
}
WINAPI(GetCommandLineW)

__winfnc const char *GetEnvironmentStringsA() {
    //Determine environment block size
    int len = 1;
    for(const char **p = winmodule_get_cur()->environ; *p; p++) len += strlen(*p) + 1;

    //Create environment block
    char *env = (char*) malloc(len);
    if(!env) { winerr_set_errno(); return NULL; }

    char *d = env;
    for(const char **p = winmodule_get_cur()->environ; *p; p++) {
        int l = strlen(*p);
        memcpy(d, *p, l+1);
        d += l + 1;
    }
    *d = 0;

    return env;
}
WINAPI(GetEnvironmentStringsA)

__winfnc BOOL FreeEnvironmentStringsA(const char *env) {
    free((void*) env);
    return TRUE;
}
WINAPI(FreeEnvironmentStringsA)

__winfnc const char16_t *GetEnvironmentStringsW() {
    //Determine environment block size
    int len = 1;
    for(const char **p = winmodule_get_cur()->environ; *p; p++) len += strlen(*p) + 1;

    //Create environment block
    char16_t *env = (char16_t*) malloc(len * sizeof(char16_t));
    if(!env) { winerr_set_errno(); return NULL; }

    char16_t *d = env;
    for(const char **p = winmodule_get_cur()->environ; *p; p++) {
        char16_t *wp = winstr_from_str(*p);
        int l = winstr_len(wp);
        memcpy(d, *p, (l+1) * sizeof(char16_t));
        d += l + 1;
        free(wp);
    }
    *d = 0;

    return env;
}
WINAPI(GetEnvironmentStringsW)

__winfnc BOOL FreeEnvironmentStringsW(const char16_t *env) {
    free((void*) env);
    return TRUE;
}
WINAPI(FreeEnvironmentStringsW)

__winfnc BOOL IsProcessorFeaturePresent(DWORD feature) {
    log_debug("IsProcessorFeaturePresent | feature: %d", feature);
    return FALSE;
}
WINAPI(IsProcessorFeaturePresent)

//Prefix of the real AMD64 CONTEXT struct layout (winnt.h), up through the general-purpose
//registers. The caller always allocates a full-size real CONTEXT (its compiler knows the true,
//much larger size including FP/vector state), so writing only this prefix and leaving the rest
//of that allocation untouched is safe.
typedef struct {
    DWORD64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    DWORD64 R8, R9, R10, R11, R12, R13, R14, R15;
    DWORD64 Rip;
} AMD64_CONTEXT_PREFIX;

//The driver's CRT only calls this from its stack-buffer-overrun reporting path (__report_gsfailure:
//RtlCaptureContext -> RtlLookupFunctionEntry -> RtlVirtualUnwind, right before fast-failing), i.e.
//after a stack cookie check has already failed. Its security cookie setup doesn't use it. So no real
//register capture is needed - just a well-formed x86-64 context: zeroed, with the fixed user-mode
//segment/flag values and the caller's real stack and instruction pointers.
__winfnc void RtlCaptureContext(AMD64_CONTEXT_PREFIX *context) {
    log_warn("RtlCaptureContext called - the driver is likely reporting a stack buffer overrun!");
    *context = (AMD64_CONTEXT_PREFIX) {0};
    context->MxCsr = 0x1f80;
    context->SegCs = 0x33;
    context->SegSs = 0x2b;
    context->EFlags = 0x202;
    context->Rsp = (DWORD64) (uintptr_t) __builtin_frame_address(0);
    context->Rip = (DWORD64) (uintptr_t) __builtin_return_address(0);
}
WINAPI(RtlCaptureContext)

__winfnc void *RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *image_base, void *history) {
    return NULL;
}
WINAPI(RtlLookupFunctionEntry)

__winfnc BOOL IsDebuggerPresent() {
    return FALSE;
}
WINAPI(IsDebuggerPresent)

typedef LONG __winfnc TOP_LEVEL_EXCEPTION_FILTER(void *ExceptionPointers);

static TOP_LEVEL_EXCEPTION_FILTER *excep_filter = NULL;

__winfnc TOP_LEVEL_EXCEPTION_FILTER *SetUnhandledExceptionFilter(TOP_LEVEL_EXCEPTION_FILTER *filter) {
    TOP_LEVEL_EXCEPTION_FILTER *old_filter = excep_filter;
    excep_filter = filter;
    return old_filter;
}
WINAPI(SetUnhandledExceptionFilter)

__winfnc LONG UnhandledExceptionFilter(void *excep_pointers) {
    if(excep_filter) {
        LONG ret = excep_filter(excep_pointers);
        if(ret == 0 || ret == 1) return ret;
    }
    return 1;
}
WINAPI(UnhandledExceptionFilter)

__winfnc void Sleep(DWORD num_ms) {
    cant_fail(usleep((useconds_t) num_ms * 1000));
}
WINAPI(Sleep)