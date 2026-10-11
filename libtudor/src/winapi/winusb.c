#include <libusb.h>
#include "internal.h"

//Real WinUsb.dll API implementation, backed by libusb. The driver resolves these via
//LoadLibraryA+GetProcAddress during OnPrepareHardware and does all of its sensor I/O through them
//(see FINDINGS.md). DeviceHandle is ignored; WinUsb_Initialize operates on the one active libusb
//device instead, since this project only ever talks to one physical sensor per process.

struct winusb_pipe_info {
    UCHAR pipe_id;
    UCHAR type; //0=control 1=isoch 2=bulk 3=interrupt (matches libusb_transfer_type numbering)
    USHORT max_packet_size;
    UCHAR interval;
    ULONG timeout_ms; //PIPE_TRANSFER_TIMEOUT policy - 0 means no timeout, same as real WinUsb
};

struct winusb_handle {
    libusb_device_handle *usb_dev;
    UCHAR interface_number;
    struct winusb_pipe_info pipes[16];
    int num_pipes;
};

static libusb_device_handle *g_active_usb_dev;

void winusb_set_active_device(libusb_device_handle *usb_dev) {
    g_active_usb_dev = usb_dev;
}

//Exposed for setupapi.c's fake CreateFileA/W, which needs to hand back a stable, non-invalid
//handle value representing "the one active device" - same underlying device this file already
//tracks for WinUsb_Initialize's own use.
libusb_device_handle *tudor_get_active_usb_device(void) {
    return g_active_usb_dev;
}

typedef struct {
    UCHAR bLength, bDescriptorType;
    UCHAR bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    UCHAR bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} WINUSB_USB_INTERFACE_DESCRIPTOR;

typedef struct {
    ULONG PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
} WINUSB_PIPE_INFORMATION;

typedef struct {
    UCHAR RequestType, Request;
    USHORT Value, Index, Length;
} WINUSB_SETUP_PACKET;

__winfnc BOOL WinUsb_Initialize(HANDLE DeviceHandle, void **InterfaceHandle) {
    if(!g_active_usb_dev) {
        log_warn("WinUsb_Initialize called with no active USB device!");
        winerr_set_code(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    struct winusb_handle *wu = (struct winusb_handle*) malloc(sizeof(struct winusb_handle));
    if(!wu) { winerr_set_errno(); return FALSE; }
    wu->usb_dev = g_active_usb_dev;
    wu->interface_number = 0;
    wu->num_pipes = 0;

    int err;
    if((err = libusb_claim_interface(wu->usb_dev, wu->interface_number)) != 0) {
        log_error("WinUsb_Initialize: libusb_claim_interface failed: %d [%s]", err, libusb_error_name(err));
        free(wu);
        winerr_set_code(ERROR_GEN_FAILURE);
        return FALSE;
    }

    struct libusb_config_descriptor *cfg;
    if((err = libusb_get_active_config_descriptor(libusb_get_device(wu->usb_dev), &cfg)) != 0) {
        log_error("WinUsb_Initialize: libusb_get_active_config_descriptor failed: %d [%s]", err, libusb_error_name(err));
        libusb_release_interface(wu->usb_dev, wu->interface_number);
        free(wu);
        winerr_set_code(ERROR_GEN_FAILURE);
        return FALSE;
    }

    const struct libusb_interface_descriptor *if_descr = &cfg->interface[wu->interface_number].altsetting[0];
    wu->num_pipes = if_descr->bNumEndpoints;
    if(wu->num_pipes > 16) wu->num_pipes = 16;
    for(int i = 0; i < wu->num_pipes; i++) {
        const struct libusb_endpoint_descriptor *ep = &if_descr->endpoint[i];
        wu->pipes[i] = (struct winusb_pipe_info) {
            .pipe_id = ep->bEndpointAddress,
            .type = ep->bmAttributes & 0x3,
            .max_packet_size = ep->wMaxPacketSize,
            .interval = ep->bInterval,
        };
    }
    libusb_free_config_descriptor(cfg);

    log_warn("WinUsb_Initialize: claimed interface %d, %d pipes", wu->interface_number, wu->num_pipes);
    *InterfaceHandle = wu;
    winerr_clear();
    return TRUE;
}
WINAPI(WinUsb_Initialize)

__winfnc BOOL WinUsb_Free(void *InterfaceHandle) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(wu) {
        libusb_release_interface(wu->usb_dev, wu->interface_number);
        free(wu);
    }
    return TRUE;
}
WINAPI(WinUsb_Free)

__winfnc BOOL WinUsb_GetDescriptor(void *InterfaceHandle, UCHAR DescriptorType, UCHAR Index, USHORT LanguageID, UCHAR *Buffer, ULONG BufferLength, ULONG *LengthTransferred) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }

    int ret = libusb_get_descriptor(wu->usb_dev, DescriptorType, Index, Buffer, (int) BufferLength);
    if(ret < 0) {
        log_error("WinUsb_GetDescriptor failed: %d [%s]", ret, libusb_error_name(ret));
        winerr_set_code(ERROR_GEN_FAILURE);
        return FALSE;
    }

    if(LengthTransferred) *LengthTransferred = (ULONG) ret;
    return TRUE;
}
WINAPI(WinUsb_GetDescriptor)

__winfnc BOOL WinUsb_QueryInterfaceSettings(void *InterfaceHandle, UCHAR AlternateInterfaceNumber, WINUSB_USB_INTERFACE_DESCRIPTOR *UsbAltInterfaceDescriptor) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }

    struct libusb_config_descriptor *cfg;
    int err = libusb_get_active_config_descriptor(libusb_get_device(wu->usb_dev), &cfg);
    if(err != 0) { winerr_set_code(ERROR_GEN_FAILURE); return FALSE; }

    if(AlternateInterfaceNumber >= cfg->interface[wu->interface_number].num_altsetting) {
        libusb_free_config_descriptor(cfg);
        winerr_set_code(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    const struct libusb_interface_descriptor *if_descr = &cfg->interface[wu->interface_number].altsetting[AlternateInterfaceNumber];
    *UsbAltInterfaceDescriptor = (WINUSB_USB_INTERFACE_DESCRIPTOR) {
        .bLength = if_descr->bLength, .bDescriptorType = if_descr->bDescriptorType,
        .bInterfaceNumber = if_descr->bInterfaceNumber, .bAlternateSetting = if_descr->bAlternateSetting,
        .bNumEndpoints = if_descr->bNumEndpoints, .bInterfaceClass = if_descr->bInterfaceClass,
        .bInterfaceSubClass = if_descr->bInterfaceSubClass, .bInterfaceProtocol = if_descr->bInterfaceProtocol,
        .iInterface = if_descr->iInterface,
    };
    libusb_free_config_descriptor(cfg);
    return TRUE;
}
WINAPI(WinUsb_QueryInterfaceSettings)

__winfnc BOOL WinUsb_QueryPipe(void *InterfaceHandle, UCHAR AlternateInterfaceNumber, UCHAR PipeIndex, WINUSB_PIPE_INFORMATION *PipeInformation) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }
    if(PipeIndex >= wu->num_pipes) { winerr_set_code(ERROR_NO_MORE_ITEMS); return FALSE; }

    struct winusb_pipe_info *p = &wu->pipes[PipeIndex];
    *PipeInformation = (WINUSB_PIPE_INFORMATION) {
        .PipeType = p->type, .PipeId = p->pipe_id,
        .MaximumPacketSize = p->max_packet_size, .Interval = p->interval,
    };
    return TRUE;
}
WINAPI(WinUsb_QueryPipe)

static struct winusb_pipe_info *winusb_find_pipe(struct winusb_handle *wu, UCHAR pipe_id) {
    for(int i = 0; i < wu->num_pipes; i++) if(wu->pipes[i].pipe_id == pipe_id) return &wu->pipes[i];
    return NULL;
}

//Real WinUsb_ReadPipe/WritePipe support both synchronous (Overlapped == NULL) and asynchronous use.
//Both are backed by libusb async transfers (the CLI / tudor-host both run a libusb event thread), so
//that every in-flight transfer - including a blocking synchronous one - can be cancelled through
//WinUsb_AbortPipe, and timeouts follow the PIPE_TRANSFER_TIMEOUT policy exactly like real WinUsb (0,
//the default, means no timeout). This matters for the interrupt endpoint: the driver's interrupt
//thread does a synchronous read on it that only completes once the sensor raises an event (e.g. a
//finger touch), and aborts it via WinUsb_AbortPipe when a capture is cancelled. Asynchronous
//transfers complete their OVERLAPPED (Internal/InternalHigh + hEvent) from the libusb callback.
#define PIPE_TRANSFER_TIMEOUT 0x03
#define WINUSB_CONTROL_TIMEOUT_MS 5000

#define STATUS_IO_TIMEOUT 0xc00000b5
#define STATUS_UNSUCCESSFUL 0xc0000001

struct winusb_async_xfer {
    struct winusb_async_xfer *prev, *next;
    struct libusb_transfer *transfer;
    UCHAR pipe_id;
    OVERLAPPED *ovlp; //NULL for synchronous transfers, which wait on the fields below instead

    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
};

static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winusb_async_xfer *g_async_head = NULL;

static void winusb_log_data(const UCHAR *buf, int len) {
    if(LOG_LEVEL > LOG_VERBOSE || len <= 0) return;
    char hex[2*64+4];
    int n = len < 64 ? len : 64;
    for(int i = 0; i < n; i++) sprintf(hex + 2*i, "%02x", buf[i]);
    if(n < len) strcpy(hex + 2*n, "...");
    log_verbose("  data: %s", hex);
}

static NTSTATUS winusb_xfer_status(enum libusb_transfer_status status) {
    switch(status) {
        case LIBUSB_TRANSFER_COMPLETED: return STATUS_SUCCESS;
        case LIBUSB_TRANSFER_CANCELLED: return STATUS_CANCELLED;
        case LIBUSB_TRANSFER_TIMED_OUT: return STATUS_IO_TIMEOUT;
        default: return STATUS_UNSUCCESSFUL;
    }
}

static void LIBUSB_CALL winusb_async_cb(struct libusb_transfer *transfer) {
    struct winusb_async_xfer *x = (struct winusb_async_xfer*) transfer->user_data;

    cant_fail_ret(pthread_mutex_lock(&g_async_lock));
    if(x->prev) x->prev->next = x->next;
    else g_async_head = x->next;
    if(x->next) x->next->prev = x->prev;
    cant_fail_ret(pthread_mutex_unlock(&g_async_lock));

    if(!x->ovlp) {
        //Synchronous transfer - the submitting thread owns and frees everything
        cant_fail_ret(pthread_mutex_lock(&x->lock));
        x->done = true;
        cant_fail_ret(pthread_cond_broadcast(&x->cond));
        cant_fail_ret(pthread_mutex_unlock(&x->lock));
        return;
    }

    NTSTATUS status = winusb_xfer_status(transfer->status);
    log_debug("WinUsb async transfer on pipe 0x%02x completed: libusb status %d, %d bytes -> NTSTATUS 0x%x",
        x->pipe_id, transfer->status, transfer->actual_length, status);
    winusb_log_data(transfer->buffer, transfer->actual_length);

    OVERLAPPED *ovlp = x->ovlp;
    ovlp->InternalHigh = (ULONG_PTR) transfer->actual_length;
    __atomic_store_n(&ovlp->Internal, (ULONG_PTR) status, __ATOMIC_RELEASE);
    if(ovlp->hEvent) win_set_event(ovlp->hEvent);

    libusb_free_transfer(transfer);
    free(x);
}

static BOOL winusb_transfer(struct winusb_handle *wu, UCHAR PipeID, UCHAR *Buffer, ULONG BufferLength, ULONG *LengthTransferred, OVERLAPPED *Overlapped, const char *dir) {
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }
    struct winusb_pipe_info *p = winusb_find_pipe(wu, PipeID);
    if(!p) { winerr_set_code(ERROR_INVALID_PARAMETER); return FALSE; }

    struct winusb_async_xfer *x = (struct winusb_async_xfer*) malloc(sizeof(struct winusb_async_xfer));
    if(!x) { perror("Couldn't allocate WinUsb transfer"); abort(); }
    x->prev = NULL;
    x->pipe_id = PipeID;
    x->ovlp = Overlapped;
    x->done = false;
    x->transfer = libusb_alloc_transfer(0);
    if(!x->transfer) { perror("Couldn't allocate libusb transfer"); abort(); }
    if(!Overlapped) {
        cant_fail_ret(pthread_mutex_init(&x->lock, NULL));
        cant_fail_ret(pthread_cond_init(&x->cond, NULL));
    }

    if(p->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) libusb_fill_interrupt_transfer(x->transfer, wu->usb_dev, PipeID, Buffer, (int) BufferLength, winusb_async_cb, x, p->timeout_ms);
    else libusb_fill_bulk_transfer(x->transfer, wu->usb_dev, PipeID, Buffer, (int) BufferLength, winusb_async_cb, x, p->timeout_ms);

    if(Overlapped) {
        Overlapped->Internal = STATUS_PENDING;
        Overlapped->InternalHigh = 0;
        if(Overlapped->hEvent) win_reset_event(Overlapped->hEvent);
    }

    cant_fail_ret(pthread_mutex_lock(&g_async_lock));
    x->next = g_async_head;
    if(g_async_head) g_async_head->prev = x;
    g_async_head = x;
    int err = libusb_submit_transfer(x->transfer);
    if(err != 0) {
        g_async_head = x->next;
        if(x->next) x->next->prev = NULL;
    }
    cant_fail_ret(pthread_mutex_unlock(&g_async_lock));

    if(err != 0) {
        log_error("WinUsb_%sPipe(0x%02x, %u bytes): libusb_submit_transfer failed: %d [%s]", dir, PipeID, BufferLength, err, libusb_error_name(err));
        libusb_free_transfer(x->transfer);
        if(!Overlapped) {
            pthread_cond_destroy(&x->cond);
            pthread_mutex_destroy(&x->lock);
        } else Overlapped->Internal = STATUS_UNSUCCESSFUL;
        free(x);
        winerr_set_code(ERROR_GEN_FAILURE);
        return FALSE;
    }

    if(Overlapped) {
        log_debug("WinUsb_%sPipe(0x%02x, %u bytes) submitted async [ovlp=%p evt=%p timeout=%u]", dir, PipeID, BufferLength, Overlapped, Overlapped->hEvent, p->timeout_ms);
        if(LengthTransferred) *LengthTransferred = 0;
        winerr_set_code(ERROR_IO_PENDING);
        return FALSE;
    }

    //Synchronous - wait for the libusb callback
    cant_fail_ret(pthread_mutex_lock(&x->lock));
    while(!x->done) cant_fail_ret(pthread_cond_wait(&x->cond, &x->lock));
    cant_fail_ret(pthread_mutex_unlock(&x->lock));
    pthread_cond_destroy(&x->cond);
    pthread_mutex_destroy(&x->lock);

    enum libusb_transfer_status xfer_status = x->transfer->status;
    int transferred = x->transfer->actual_length;
    libusb_free_transfer(x->transfer);
    free(x);

    if(LengthTransferred) *LengthTransferred = (ULONG) transferred;
    if(xfer_status != LIBUSB_TRANSFER_COMPLETED) {
        log_debug("WinUsb_%sPipe(0x%02x, %u bytes) -> libusb transfer status %d", dir, PipeID, BufferLength, xfer_status);
        switch(xfer_status) {
            case LIBUSB_TRANSFER_TIMED_OUT: winerr_set_code(ERROR_SEM_TIMEOUT); break;
            case LIBUSB_TRANSFER_CANCELLED: winerr_set_code(ERROR_OPERATION_ABORTED); break;
            default: winerr_set_code(ERROR_GEN_FAILURE); break;
        }
        return FALSE;
    }
    log_debug("WinUsb_%sPipe(0x%02x) got %d bytes", dir, PipeID, transferred);
    winusb_log_data(Buffer, transferred);
    return TRUE;
}

__winfnc BOOL WinUsb_ReadPipe(void *InterfaceHandle, UCHAR PipeID, UCHAR *Buffer, ULONG BufferLength, ULONG *LengthTransferred, OVERLAPPED *Overlapped) {
    return winusb_transfer((struct winusb_handle*) InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped, "Read");
}
WINAPI(WinUsb_ReadPipe)

__winfnc BOOL WinUsb_WritePipe(void *InterfaceHandle, UCHAR PipeID, UCHAR *Buffer, ULONG BufferLength, ULONG *LengthTransferred, OVERLAPPED *Overlapped) {
    return winusb_transfer((struct winusb_handle*) InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped, "Write");
}
WINAPI(WinUsb_WritePipe)

__winfnc BOOL WinUsb_GetOverlappedResult(void *InterfaceHandle, OVERLAPPED *Overlapped, ULONG *NumberOfBytesTransferred, BOOL Wait) {
    while(__atomic_load_n(&Overlapped->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING) {
        if(!Wait) { winerr_set_code(ERROR_IO_INCOMPLETE); return FALSE; }
        win_wait_sync_obj(Overlapped->hEvent, INFINITE);
    }
    if(NumberOfBytesTransferred) *NumberOfBytesTransferred = (ULONG) Overlapped->InternalHigh;
    switch(Overlapped->Internal) {
        case STATUS_SUCCESS: return TRUE;
        case STATUS_CANCELLED: winerr_set_code(ERROR_OPERATION_ABORTED); return FALSE;
        case STATUS_IO_TIMEOUT: winerr_set_code(ERROR_SEM_TIMEOUT); return FALSE;
        default: winerr_set_code(ERROR_GEN_FAILURE); return FALSE;
    }
}
WINAPI(WinUsb_GetOverlappedResult)

__winfnc BOOL WinUsb_ControlTransfer(void *InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket, UCHAR *Buffer, ULONG BufferLength, ULONG *LengthTransferred, OVERLAPPED *Overlapped) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }

    int ret = libusb_control_transfer(wu->usb_dev, SetupPacket.RequestType, SetupPacket.Request,
        SetupPacket.Value, SetupPacket.Index, Buffer, SetupPacket.Length, WINUSB_CONTROL_TIMEOUT_MS);
    if(ret < 0) {
        log_debug("WinUsb_ControlTransfer failed: %d [%s]", ret, libusb_error_name(ret));
        winerr_set_code(ERROR_GEN_FAILURE);
        return FALSE;
    }

    if(LengthTransferred) *LengthTransferred = (ULONG) ret;
    return TRUE;
}
WINAPI(WinUsb_ControlTransfer)

__winfnc BOOL WinUsb_AbortPipe(void *InterfaceHandle, UCHAR PipeID) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }
    log_debug("WinUsb_AbortPipe(0x%02x)", PipeID);
    cant_fail_ret(pthread_mutex_lock(&g_async_lock));
    for(struct winusb_async_xfer *x = g_async_head; x; x = x->next) {
        if(x->pipe_id == PipeID) libusb_cancel_transfer(x->transfer);
    }
    cant_fail_ret(pthread_mutex_unlock(&g_async_lock));
    return TRUE;
}
WINAPI(WinUsb_AbortPipe)

__winfnc BOOL WinUsb_ResetPipe(void *InterfaceHandle, UCHAR PipeID) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    if(!wu) { winerr_set_code(ERROR_INVALID_HANDLE); return FALSE; }
    int err = libusb_clear_halt(wu->usb_dev, PipeID);
    if(err != 0 && err != LIBUSB_ERROR_NOT_FOUND) { winerr_set_code(ERROR_GEN_FAILURE); return FALSE; }
    return TRUE;
}
WINAPI(WinUsb_ResetPipe)

__winfnc BOOL WinUsb_SetPipePolicy(void *InterfaceHandle, UCHAR PipeID, ULONG PolicyType, ULONG ValueLength, void *Value) {
    struct winusb_handle *wu = (struct winusb_handle*) InterfaceHandle;
    log_debug("WinUsb_SetPipePolicy(0x%02x, policy=0x%x, len=%u, val=0x%x)", PipeID, PolicyType, ValueLength, (Value && ValueLength >= 4) ? *(ULONG*) Value : (Value && ValueLength) ? *(UCHAR*) Value : 0);
    struct winusb_pipe_info *p = wu ? winusb_find_pipe(wu, PipeID) : NULL;
    if(p && PolicyType == PIPE_TRANSFER_TIMEOUT && Value && ValueLength >= 4) p->timeout_ms = *(ULONG*) Value;
    return TRUE;
}
WINAPI(WinUsb_SetPipePolicy)

__winfnc BOOL WinUsb_GetPipePolicy(void *InterfaceHandle, UCHAR PipeID, ULONG PolicyType, ULONG *ValueLength, void *Value) {
    if(ValueLength) *ValueLength = 0;
    return TRUE;
}
WINAPI(WinUsb_GetPipePolicy)

__winfnc BOOL WinUsb_SetPowerPolicy(void *InterfaceHandle, ULONG PolicyType, ULONG ValueLength, void *Value) { return TRUE; }
WINAPI(WinUsb_SetPowerPolicy)

__winfnc BOOL WinUsb_GetPowerPolicy(void *InterfaceHandle, ULONG PolicyType, ULONG *ValueLength, void *Value) {
    if(ValueLength) *ValueLength = 0;
    return TRUE;
}
WINAPI(WinUsb_GetPowerPolicy)
