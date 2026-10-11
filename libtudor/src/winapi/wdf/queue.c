#include "internal.h"

struct winwdf_queue {
    struct wdf_object object;
    struct winwdf_device *device;

    WDF_IO_QUEUE_CONFIG config;

    bool stopped;

    //Bridge state for queues created via tudor/com_activate.c's IWDFDevice::CreateIoQueue - NULL
    //for queues created through the normal flat WdfIoQueueCreate path. com_devctrl_callback is the
    //driver's own real IQueueCallbackDeviceIoControl COM object (already AddRef'd); com_facade is
    //this queue's own persistent COM wrapper, handed back to that callback as its IWDFIoQueue arg
    //(the callback doesn't inspect it, just passes it along, same "opaque token" pattern as
    //IWDFDeviceInitialize elsewhere in the COM bridge).
    void *com_devctrl_callback;
    void *com_facade;
};

void wdf_queue_set_com_bridge(struct winwdf_queue *queue, void *devctrl_callback, void *com_facade) {
    queue->com_devctrl_callback = devctrl_callback;
    queue->com_facade = com_facade;
}
void *wdf_queue_get_com_devctrl_callback(struct winwdf_queue *queue) { return queue->com_devctrl_callback; }
void *wdf_queue_get_com_facade(struct winwdf_queue *queue) { return queue->com_facade; }

static void queue_destr(struct winwdf_queue *queue) {
    //Stop the queue
    if(queue->config.PowerManaged && !queue->stopped && queue->config.EvtIoStop) queue->config.EvtIoStop(&queue->object, NULL, WdfRequestStopActionSuspend); 

    //Remove queue from device
    wdf_remove_device_queue(queue->device, queue);

    //Free memory
    wdf_cleanup_obj(&queue->object);
    free(queue);
}

void wdf_queue_resume(struct winwdf_queue *queue) {
    if(!queue->config.PowerManaged || !queue->stopped) return;
    if(queue->config.EvtIoResume) queue->config.EvtIoResume(&queue->object, NULL);
    queue->stopped = false;
}

void wdf_queue_stop(struct winwdf_queue *queue) {
    if(!queue->config.PowerManaged || queue->stopped) return;
    if(queue->config.EvtIoStop) queue->config.EvtIoStop(&queue->object, NULL, WdfRequestStopActionSuspend);
    queue->stopped = true;
}

void wdf_queue_create(struct winwdf_queue *queue, struct winwdf_request *req) {
    if(queue->stopped || !queue->config.EvtIoDefault) {
        log_warn("Couldn't process WDF queue create request!");
    } else queue->config.EvtIoDefault(&queue->object, (WDFOBJECT) req);
}

void wdf_queue_read(struct winwdf_queue *queue, struct winwdf_request *req, size_t len) {
    if(queue->stopped || !queue->config.EvtIoRead) {
        log_warn("Couldn't process WDF queue read request!");
    } else queue->config.EvtIoRead(&queue->object, (WDFOBJECT) req, len);
}

void wdf_queue_write(struct winwdf_queue *queue, struct winwdf_request *req, size_t len) {
    if(queue->stopped || !queue->config.EvtIoWrite) {
        log_warn("Couldn't process WDF queue write request!");
    } else queue->config.EvtIoWrite(&queue->object, (WDFOBJECT) req, len);
}

void wdf_queue_devctrl(struct winwdf_queue *queue, struct winwdf_request *req, ULONG code, size_t in_len, size_t out_len) {
    if(queue->stopped || !queue->config.EvtIoDeviceControl) {
        log_warn("Couldn't process WDF queue devctrl request!");
    } else queue->config.EvtIoDeviceControl(&queue->object, (WDFOBJECT) req, out_len, in_len, code);
}

void wdf_queue_int_devctrl(struct winwdf_queue *queue, struct winwdf_request *req, ULONG code, size_t in_len, size_t out_len) {
    if(queue->stopped || !queue->config.EvtIoInternalDeviceControl) {
        log_warn("Couldn't process WDF queue internal devctrl request!");
    } else queue->config.EvtIoInternalDeviceControl(&queue->object, (WDFOBJECT) req, out_len, in_len, code);
}

__winfnc NTSTATUS WdfIoQueueCreate(WDF_DRIVER_GLOBALS *globals, WDFOBJECT device, WDF_IO_QUEUE_CONFIG *config, WDF_OBJECT_ATTRIBUTES *obj_attrs, WDFOBJECT *out) {
    if(config->Size < sizeof(WDF_IO_QUEUE_CONFIG)) return WINERR_SET_CODE;

    //Create the queue object
    struct winwdf_queue *queue = (struct winwdf_queue*) malloc(sizeof(struct winwdf_queue));
    if(!queue) return winerr_from_errno();

    wdf_create_obj((struct wdf_object*) winwdf_get_driver(globals), &queue->object, (wdf_obj_destr_fnc*) queue_destr, obj_attrs);
    queue->device = (struct winwdf_device*) device;
    queue->config = *config;

    queue->stopped = false;
    queue->com_devctrl_callback = NULL;
    queue->com_facade = NULL;

    //Add queue to device
    wdf_add_device_queue(queue->device, queue);

    if(out) *out = &queue->object;
    log_verbose("WdfIoQueueCreate queue=%p DispatchType=%u EvtIoDeviceControl=%p", queue, config->DispatchType, (void*) config->EvtIoDeviceControl);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfIoQueueCreate, 85)

//WdfIoQueueRetrieveNextRequest - identified by table-index gap (85=WdfIoQueueCreate,
//87=WdfIoQueueStart, both already verified live; 86 is the only IoQueue-group slot between them,
//and this driver's own background worker thread was observed live calling straight through this
//exact slot to pull a request off a manually-dispatched queue). This project's queue bridge is
//push-style only (com_activate.c dispatches requests directly to the driver's COM callback as they
//arrive, via WdfIoQueueCreate's EvtIoDeviceControl - see com_queue_evt_io_device_control) and never
//maintains an internal pending-request list a caller could pull from, so nothing is ever "waiting"
//here from this function's perspective. Without a real implementation, the driver's worker thread
//dereferenced this call's return value directly as if it were a real IWDFIoRequest-like object -
//crashing (observed live: SIGSEGV reading a vtable slot off return value 1). Returning
//STATUS_NO_MORE_ITEMS matches real WDF's own documented behavior for an empty manual queue, letting
//the driver's own (real, unmodified) code handle "nothing to do yet" instead of dereferencing our
//stub's return value as if it were a request.
__winfnc NTSTATUS WdfIoQueueRetrieveNextRequest(WDF_DRIVER_GLOBALS *globals, WDFOBJECT queue_obj, WDFOBJECT *out_request) {
    log_verbose("WdfIoQueueRetrieveNextRequest queue=%p", queue_obj);
    if(out_request) *out_request = NULL;
    return 0x8000001A; /* STATUS_NO_MORE_ITEMS */
}
WDFFUNC(WdfIoQueueRetrieveNextRequest, 86)

__winfnc NTSTATUS WdfDeviceConfigureRequestDispatching(WDF_DRIVER_GLOBALS *globals, WDFOBJECT device, WDFOBJECT queue_obj, WDF_REQUEST_TYPE req_type) {
    struct winwdf_queue *queue = (struct winwdf_queue*) queue_obj;

    if(queue->device != (struct winwdf_device*) device) return WINERR_SET_CODE;

    //Translate the request type
    enum wdf_queue_req_type req;
    switch(req_type) {
        case WdfRequestTypeCreate: req = WDF_QUEUE_REQ_CREATE; break;
        case WdfRequestTypeRead: req = WDF_QUEUE_REQ_READ; break;
        case WdfRequestTypeWrite: req = WDF_QUEUE_REQ_WRITE; break;
        case WdfRequestTypeDeviceControl: req = WDF_QUEUE_REQ_DEVCTRL; break;
        case WdfRequestTypeDeviceControlInternal: req = WDF_QUEUE_REQ_DEVCTRL_INT; break;
        default: return WINERR_SET_CODE;
    }

    //Configure the dispatch
    wdf_configure_device_dispatch(queue->device, queue, req);

    return STATUS_SUCCESS;
}
WDFFUNC(WdfDeviceConfigureRequestDispatching, 40)

__winfnc void WdfIoQueueStart(WDF_DRIVER_GLOBALS *globals, WDFOBJECT queue_obj) {
    
}
WDFFUNC(WdfIoQueueStart, 87)