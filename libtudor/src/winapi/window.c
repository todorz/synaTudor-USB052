#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "internal.h"

//Minimal Win32 window/message-queue subsystem. Drivers that create a hidden "message-only" window
//(commonly just to receive broadcast notifications like WM_DEVICECHANGE) need enough of this to
//register a class, create a window, and run a GetMessage/DispatchMessage loop - not full GDI/user
//interface support, which this driver (running headless) has no real use for.

typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;

typedef struct {
    HANDLE hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG;

typedef LRESULT __winfnc (*WNDPROC)(HANDLE hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

typedef struct {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HANDLE hInstance;
    HANDLE hIcon;
    HANDLE hCursor;
    HANDLE hbrBackground;
    const char16_t *lpszMenuName;
    const char16_t *lpszClassName;
    HANDLE hIconSm;
} WNDCLASSEXW;

#define WM_DESTROY 0x0002
#define WM_QUIT 0x0012
#define PM_REMOVE 0x0001

//<<<<< Window classes >>>>>

struct wndclass {
    struct wndclass *next;
    char *name;
    WNDPROC wndproc;
};

static pthread_rwlock_t wndclasses_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct wndclass *wndclasses_head = NULL;

static WNDPROC find_wndclass(const char *name) {
    cant_fail_ret(pthread_rwlock_rdlock(&wndclasses_lock));
    WNDPROC wndproc = NULL;
    for(struct wndclass *c = wndclasses_head; c != NULL; c = c->next) {
        if(strcmp(c->name, name) == 0) { wndproc = c->wndproc; break; }
    }
    cant_fail_ret(pthread_rwlock_unlock(&wndclasses_lock));
    return wndproc;
}

__winfnc WORD RegisterClassExW(const WNDCLASSEXW *wnd_class) {
    struct wndclass *c = (struct wndclass*) malloc(sizeof(struct wndclass));
    if(!c) { winerr_from_errno(); return 0; }

    c->name = winstr_to_str(wnd_class->lpszClassName);
    c->wndproc = wnd_class->lpfnWndProc;

    cant_fail_ret(pthread_rwlock_wrlock(&wndclasses_lock));
    c->next = wndclasses_head;
    wndclasses_head = c;
    cant_fail_ret(pthread_rwlock_unlock(&wndclasses_lock));

    return 1; //A real (nonzero) ATOM value; nothing currently looks it up by value.
}
WINAPI(RegisterClassExW)

//<<<<< Per-thread message queues >>>>>

struct msg_queue_entry {
    struct msg_queue_entry *next;
    MSG msg;
};

struct msg_queue {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct msg_queue_entry *head, *tail;
};

static __thread struct msg_queue *cur_queue = NULL;

//Thread ID -> queue registry, so PostThreadMessageW can reach another thread's queue (the driver
//uses it to tell its hidden power-notification window thread to quit during OnReleaseHardware).
//Like real Windows, a thread only has a queue once it has called a message function.
struct queue_reg_entry { struct queue_reg_entry *next; DWORD thread_id; struct msg_queue *queue; };
static pthread_mutex_t queue_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct queue_reg_entry *queue_reg_head = NULL;
static pthread_key_t queue_reg_key;
static pthread_once_t queue_reg_once = PTHREAD_ONCE_INIT;

static void queue_unregister(void *queue) {
    cant_fail_ret(pthread_mutex_lock(&queue_reg_lock));
    for(struct queue_reg_entry **pe = &queue_reg_head; *pe; pe = &(*pe)->next) {
        if((*pe)->queue != queue) continue;
        struct queue_reg_entry *e = *pe;
        *pe = e->next;
        free(e);
        break;
    }
    cant_fail_ret(pthread_mutex_unlock(&queue_reg_lock));
    //The queue itself is leaked - a poster may still hold a pointer to it
}

static void queue_reg_init() { cant_fail_ret(pthread_key_create(&queue_reg_key, queue_unregister)); }

static struct msg_queue *find_thread_queue(DWORD thread_id) {
    struct msg_queue *queue = NULL;
    cant_fail_ret(pthread_mutex_lock(&queue_reg_lock));
    for(struct queue_reg_entry *e = queue_reg_head; e; e = e->next) if(e->thread_id == thread_id) { queue = e->queue; break; }
    cant_fail_ret(pthread_mutex_unlock(&queue_reg_lock));
    return queue;
}

static struct msg_queue *get_cur_queue() {
    if(!cur_queue) {
        cur_queue = (struct msg_queue*) malloc(sizeof(struct msg_queue));
        if(!cur_queue) abort_perror("Couldn't allocate message queue");
        cant_fail_ret(pthread_mutex_init(&cur_queue->lock, NULL));
        cant_fail_ret(pthread_cond_init(&cur_queue->cond, NULL));
        cur_queue->head = cur_queue->tail = NULL;

        struct queue_reg_entry *e = (struct queue_reg_entry*) malloc(sizeof(struct queue_reg_entry));
        if(!e) abort_perror("Couldn't allocate message queue registry entry");
        e->thread_id = win_get_thread_id();
        e->queue = cur_queue;
        cant_fail_ret(pthread_once(&queue_reg_once, queue_reg_init));
        cant_fail_ret(pthread_mutex_lock(&queue_reg_lock));
        e->next = queue_reg_head;
        queue_reg_head = e;
        cant_fail_ret(pthread_mutex_unlock(&queue_reg_lock));
        cant_fail_ret(pthread_setspecific(queue_reg_key, cur_queue));
    }
    return cur_queue;
}

static void queue_push(struct msg_queue *queue, MSG msg) {
    struct msg_queue_entry *e = (struct msg_queue_entry*) malloc(sizeof(struct msg_queue_entry));
    if(!e) abort_perror("Couldn't allocate message queue entry");
    e->next = NULL;
    e->msg = msg;

    cant_fail_ret(pthread_mutex_lock(&queue->lock));
    if(queue->tail) queue->tail->next = e;
    else queue->head = e;
    queue->tail = e;
    cant_fail_ret(pthread_cond_signal(&queue->cond));
    cant_fail_ret(pthread_mutex_unlock(&queue->lock));
}

//<<<<< Windows >>>>>

struct winwindow {
    struct winwindow *next; //windows_head list, for FindWindowW
    char *class_name, *wnd_name;
    WNDPROC wndproc;
    struct msg_queue *queue; //The creating thread's queue; messages sent to this window go here.
    bool destroyed;
    intptr_t user_data; //GWLP_USERDATA - the one per-window extra slot drivers actually use in practice.
};

static pthread_mutex_t windows_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winwindow *windows_head = NULL;

typedef intptr_t LONG_PTR;
#define GWLP_WNDPROC (-4)
#define GWLP_USERDATA (-21)

__winfnc HANDLE CreateWindowExW(DWORD ex_style, const char16_t *class_name, const char16_t *wnd_name, DWORD style, int x, int y, int w, int h, HANDLE parent, HANDLE menu, HANDLE instance, void *param) {
    char *cclass_name = winstr_to_str(class_name);
    WNDPROC wndproc = find_wndclass(cclass_name);
    if(!wndproc) {
        log_warn("CreateWindowExW: Window class '%s' not registered!", cclass_name);
        free(cclass_name);
        winerr_set_code(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    struct winwindow *wnd = (struct winwindow*) malloc(sizeof(struct winwindow));
    if(!wnd) { free(cclass_name); winerr_from_errno(); return NULL; }
    wnd->class_name = cclass_name;
    wnd->wnd_name = wnd_name ? winstr_to_str(wnd_name) : NULL;
    wnd->wndproc = wndproc;
    wnd->queue = get_cur_queue();
    wnd->destroyed = false;
    wnd->user_data = 0;

    cant_fail_ret(pthread_mutex_lock(&windows_lock));
    wnd->next = windows_head;
    windows_head = wnd;
    cant_fail_ret(pthread_mutex_unlock(&windows_lock));

    return (HANDLE) wnd;
}
WINAPI(CreateWindowExW)

//Only GWLP_WNDPROC (subclassing) and GWLP_USERDATA (the common "stash a context pointer" use) are
//backed by real per-window storage - everything else (GWL_STYLE, GWLP_HINSTANCE, ...) is meaningless
//in a headless bridge with no real window manager, so unsupported indices just log and return 0
//rather than crashing outright, matching this project's generic-stub convention elsewhere.
__winfnc LONG_PTR GetWindowLongPtrW(HANDLE hwnd, int index) {
    struct winwindow *wnd = (struct winwindow*) hwnd;
    switch(index) {
        case GWLP_WNDPROC: return (LONG_PTR) (void*) wnd->wndproc;
        case GWLP_USERDATA: return wnd->user_data;
        default:
            log_warn("GetWindowLongPtrW: unsupported index %d", index);
            return 0;
    }
}
WINAPI(GetWindowLongPtrW)

__winfnc LONG_PTR SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR new_value) {
    struct winwindow *wnd = (struct winwindow*) hwnd;
    LONG_PTR old;
    switch(index) {
        case GWLP_WNDPROC: old = (LONG_PTR) (void*) wnd->wndproc; wnd->wndproc = (WNDPROC) (void*) new_value; return old;
        case GWLP_USERDATA: old = wnd->user_data; wnd->user_data = new_value; return old;
        default:
            log_warn("SetWindowLongPtrW: unsupported index %d", index);
            return 0;
    }
}
WINAPI(SetWindowLongPtrW)

__winfnc LRESULT CallWindowProcW(WNDPROC prev_proc, HANDLE hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return prev_proc(hwnd, msg, wparam, lparam);
}
WINAPI(CallWindowProcW)

__winfnc BOOL DestroyWindow(HANDLE hwnd) {
    struct winwindow *wnd = (struct winwindow*) hwnd;
    if(wnd->destroyed) return FALSE;
    wnd->destroyed = true;
    wnd->wndproc(hwnd, WM_DESTROY, 0, 0);
    return TRUE;
}
WINAPI(DestroyWindow)

//Window objects are never freed (a stale HWND must stay safe to touch), only marked destroyed - so
//this only ever matches windows that are still alive.
__winfnc HANDLE FindWindowW(const char16_t *class_name, const char16_t *wnd_name) {
    char *cclass = class_name ? winstr_to_str(class_name) : NULL, *cname = wnd_name ? winstr_to_str(wnd_name) : NULL;
    struct winwindow *found = NULL;
    cant_fail_ret(pthread_mutex_lock(&windows_lock));
    for(struct winwindow *w = windows_head; w; w = w->next) {
        if(w->destroyed) continue;
        if(cclass && strcmp(w->class_name, cclass) != 0) continue;
        if(cname && (!w->wnd_name || strcmp(w->wnd_name, cname) != 0)) continue;
        found = w;
        break;
    }
    cant_fail_ret(pthread_mutex_unlock(&windows_lock));
    log_debug("FindWindowW(class='%s', name='%s') -> %p", cclass ? cclass : "(null)", cname ? cname : "(null)", (void*) found);
    free(cclass);
    free(cname);
    return (HANDLE) found;
}
WINAPI(FindWindowW)

__winfnc LRESULT DefWindowProcW(HANDLE hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return 0;
}
WINAPI(DefWindowProcW)

//<<<<< Message loop >>>>>

__winfnc BOOL PostMessageW(HANDLE hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    struct winwindow *wnd = (struct winwindow*) hwnd;
    queue_push(wnd->queue, (MSG) { .hwnd = hwnd, .message = msg, .wParam = wparam, .lParam = lparam });
    return TRUE;
}
WINAPI(PostMessageW)

__winfnc void PostQuitMessage(int exit_code) {
    queue_push(get_cur_queue(), (MSG) { .hwnd = NULL, .message = WM_QUIT, .wParam = (WPARAM) exit_code, .lParam = 0 });
}
WINAPI(PostQuitMessage)

__winfnc BOOL PostThreadMessageW(DWORD thread_id, UINT msg, WPARAM wparam, LPARAM lparam) {
    struct msg_queue *queue = thread_id == win_get_thread_id() ? get_cur_queue() : find_thread_queue(thread_id);
    if(!queue) {
        log_debug("PostThreadMessageW: thread %u has no message queue", (unsigned) thread_id);
        winerr_set_code(ERROR_INVALID_THREAD_ID);
        return FALSE;
    }
    queue_push(queue, (MSG) { .hwnd = NULL, .message = msg, .wParam = wparam, .lParam = lparam });
    return TRUE;
}
WINAPI(PostThreadMessageW)

__winfnc LRESULT SendMessageW(HANDLE hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    //Real SendMessage can cross threads (blocking until the target thread's wndproc handles it);
    //this only supports same-thread delivery, which is what a driver's own utility window uses.
    struct winwindow *wnd = (struct winwindow*) hwnd;
    return wnd->wndproc(hwnd, msg, wparam, lparam);
}
WINAPI(SendMessageW)

__winfnc BOOL GetMessageW(MSG *out_msg, HANDLE hwnd_filter, UINT min, UINT max) {
    struct msg_queue *queue = get_cur_queue();

    cant_fail_ret(pthread_mutex_lock(&queue->lock));
    while(!queue->head) cant_fail_ret(pthread_cond_wait(&queue->cond, &queue->lock));

    struct msg_queue_entry *e = queue->head;
    queue->head = e->next;
    if(!queue->head) queue->tail = NULL;
    cant_fail_ret(pthread_mutex_unlock(&queue->lock));

    *out_msg = e->msg;
    free(e);

    return out_msg->message != WM_QUIT;
}
WINAPI(GetMessageW)

__winfnc BOOL PeekMessageW(MSG *out_msg, HANDLE hwnd_filter, UINT min, UINT max, UINT remove_msg) {
    struct msg_queue *queue = get_cur_queue();

    cant_fail_ret(pthread_mutex_lock(&queue->lock));
    struct msg_queue_entry *e = queue->head;
    if(e && (remove_msg & PM_REMOVE)) {
        queue->head = e->next;
        if(!queue->head) queue->tail = NULL;
    }
    cant_fail_ret(pthread_mutex_unlock(&queue->lock));

    if(!e) return FALSE;
    *out_msg = e->msg;
    if(remove_msg & PM_REMOVE) free(e);
    return TRUE;
}
WINAPI(PeekMessageW)

__winfnc BOOL TranslateMessage(const MSG *msg) {
    return TRUE;
}
WINAPI(TranslateMessage)

__winfnc LRESULT DispatchMessageW(const MSG *msg) {
    if(!msg->hwnd) return 0; //Thread messages (e.g. from PostThreadMessageW) have no window to dispatch to.
    struct winwindow *wnd = (struct winwindow*) msg->hwnd;
    return wnd->wndproc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}
WINAPI(DispatchMessageW)

//No real session/logon subsystem exists to generate WM_WTSSESSION_CHANGE notifications from, so
//this just reports the (no-op) registration as successful rather than failing outright.
__winfnc BOOL WTSRegisterSessionNotification(HANDLE hwnd, DWORD flags) {
    return TRUE;
}
WINAPI(WTSRegisterSessionNotification)

__winfnc BOOL WTSUnRegisterSessionNotification(HANDLE hwnd) { return TRUE; }
WINAPI(WTSUnRegisterSessionNotification)

//Same reasoning as WTSRegisterSessionNotification - there's no power manager to deliver
//WM_POWERBROADCAST settings from, so registration "succeeds" with a dummy non-NULL handle.
__winfnc void *RegisterPowerSettingNotification(HANDLE recipient, const void *setting_guid, DWORD flags) { return (void*) 1; }
WINAPI(RegisterPowerSettingNotification)

__winfnc BOOL UnregisterPowerSettingNotification(void *handle) { return TRUE; }
WINAPI(UnregisterPowerSettingNotification)

//Window classes are never freed (RegisterClassExW's entries are tiny and per-process anyway), so
//unregistering is accepted as a no-op - the driver calls it during OnReleaseHardware.
__winfnc BOOL UnregisterClassW(const char16_t *class_name, HANDLE instance) { return TRUE; }
WINAPI(UnregisterClassW)
