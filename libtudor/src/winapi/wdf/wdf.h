#ifndef LIBTUDOR_WINAPI_WDF_WDF_H
#define LIBTUDOR_WINAPI_WDF_WDF_H

#include <libusb.h>
#include "winwdf.h"

//Functions
#define NUM_WDF_FUNCS 261
extern void *wdf_functions[NUM_WDF_FUNCS];
#define WDFFUNC(fnc, idx) __constr static void __init_wdf_fnc##idx() { wdf_functions[idx] = &fnc; }

//Event queue
void winwdf_event_queue_flush();

//Objects
void winwdf_destroy_object(WDFOBJECT obj);

//Drivers
struct winwdf_driver;
struct winwdf_driver *winwdf_get_driver(WDF_DRIVER_GLOBALS *globals);
void winwdf_unload_driver(struct winwdf_driver *driver);
//For the legacy UMDF1 COM activation path (tudor/com_activate.c), which - unlike the modern
//FxDriverEntryUm flat-table path - never has the driver itself call WdfDriverCreate, since in
//that model the framework driver object is something the host constructs and hands to the driver
//via IDriverEntry::OnInitialize, not something the driver creates for itself.
struct winwdf_driver *winwdf_create_driver_for_com(HANDLE reg_key);

//Devices
struct winwdf_device;
struct wdf_device_init;
NTSTATUS winwdf_add_device(struct winwdf_driver *driver, HANDLE reg_key, libusb_device_handle *usb_dev, struct winwdf_device **dev);
void winwdf_remove_device(struct winwdf_device *device);
//Also exposed for tudor/com_activate.c's use, same reasoning as winwdf_create_driver_for_com above.
struct wdf_device_init *wdf_create_device_init(struct winwdf_driver *driver, HANDLE reg_key, libusb_device_handle *usb_dev, struct winwdf_device **out_dev);
//__winfnc/ms_abi here to match the real (driver-callable) definition's calling convention -
//WdfDeviceCreate is normally only ever invoked via the flat WDF function table by real driver
//machine code, so getting this wrong would silently pass arguments in the wrong registers.
__winfnc NTSTATUS WdfDeviceCreate(WDF_DRIVER_GLOBALS *globals, struct wdf_device_init **dev_init, WDF_OBJECT_ATTRIBUTES *obj_attrs, WDFOBJECT *out);
//Exposed for tudor/com_activate.c to build the WDF_DRIVER_GLOBALS it needs to call WdfIoQueueCreate
//directly (see the Queues section below) - a COM-activated driver's IWDFDevice facade only carries
//the raw device pointer, not a globals struct of its own.
struct winwdf_driver *wdf_device_get_driver(struct winwdf_device *dev);

//Requests
struct winwdf_request;
typedef void winwdf_request_cb_fnc(struct winwdf_request *req, NTSTATUS status, void *ctx);
bool winwdf_get_request_info(struct winwdf_request *req, NTSTATUS *status, const void **in_buf, size_t *in_buf_size, const void **out_buf, size_t *out_buf_size, ULONG_PTR *drv_info);
void winwdf_add_request_callback(struct winwdf_request *req, winwdf_request_cb_fnc *cb, void *ctx);
void winwdf_cancel_request(struct winwdf_request *req);
NTSTATUS winwdf_wait_request(struct winwdf_request *req);
//Exposed (ms_abi, first arg unused by both - see request.c) for tudor/com_activate.c's IWDFIoRequest
//bridge to complete a devctrl request on the driver's behalf, same reasoning as WdfDeviceCreate above.
__winfnc void WdfRequestSetInformation(WDF_DRIVER_GLOBALS *globals, WDFOBJECT req_obj, ULONG_PTR info);
__winfnc void WdfRequestComplete(WDF_DRIVER_GLOBALS *globals, WDFOBJECT req_obj, NTSTATUS status);
//Same, for com_activate.c's IWDFIoRequest::MarkCancelable/UnmarkCancelable bridge
__winfnc NTSTATUS WdfRequestMarkCancelable(WDF_DRIVER_GLOBALS *globals, WDFOBJECT req_obj, void __winfnc (*cancel_fnc)(WDFOBJECT req));
__winfnc NTSTATUS WdfRequestUnmarkCancelable(WDF_DRIVER_GLOBALS *globals, WDFOBJECT req_obj);

//Files
struct winwdf_file;
NTSTATUS winwdf_open_device(struct winwdf_device *device, struct winwdf_file **file);
void winwdf_close_file(struct winwdf_file *file);

//TODO NTSTATUS winwdf_read_file(struct winwdf_file *file, void *buf, size_t *size);
//TODO NTSTATUS winwdf_write_file(struct winwdf_file *file, const void *buf, size_t size);
NTSTATUS winwdf_devctrl_file(struct winwdf_file *file, ULONG code, const void *in_buf, size_t in_size, void *out_buf, size_t out_size, struct winwdf_request **req);

//Queues
typedef enum {
    WdfRequestStopActionInvalid,
    WdfRequestStopActionSuspend,
    WdfRequestStopActionPurge,
    WdfRequestStopRequestCancelable
} WDF_REQUEST_STOP_ACTION_FLAGS;

typedef void __winfnc WDF_IO_QUEUE_IO_DEFAULT(WDFOBJECT Queue, WDFOBJECT Request);
typedef void __winfnc WDF_IO_QUEUE_IO_STOP(WDFOBJECT Queue, WDFOBJECT Request, WDF_REQUEST_STOP_ACTION_FLAGS ActionFlags);
typedef void __winfnc WDF_IO_QUEUE_IO_RESUME(WDFOBJECT Queue, WDFOBJECT Request);
typedef void __winfnc WDF_IO_QUEUE_IO_READ(WDFOBJECT Queue, WDFOBJECT Request, size_t Length);
typedef void __winfnc WDF_IO_QUEUE_IO_WRITE(WDFOBJECT Queue, WDFOBJECT Request, size_t Length);
typedef void __winfnc WDF_IO_QUEUE_IO_DEVICE_CONTROL(WDFOBJECT Queue, WDFOBJECT Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode);
typedef void __winfnc WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL(WDFOBJECT Queue, WDFOBJECT Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode);
typedef void __winfnc WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE(WDFOBJECT Queue, WDFOBJECT Request);

typedef struct {
    ULONG Size;
    ULONG DispatchType;
    ULONG PowerManaged;
    BOOLEAN AllowZeroLengthRequests;
    BOOLEAN DefaultQueue;
    WDF_IO_QUEUE_IO_DEFAULT *EvtIoDefault;
    WDF_IO_QUEUE_IO_READ *EvtIoRead;
    WDF_IO_QUEUE_IO_WRITE *EvtIoWrite;
    WDF_IO_QUEUE_IO_DEVICE_CONTROL *EvtIoDeviceControl;
    WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL *EvtIoInternalDeviceControl;
    WDF_IO_QUEUE_IO_STOP *EvtIoStop;
    WDF_IO_QUEUE_IO_RESUME *EvtIoResume;
    WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE *EvtIoCanceledOnQueue;
} WDF_IO_QUEUE_CONFIG;

//Exposed for tudor/com_activate.c's UMDF1 IWDFDevice::CreateIoQueue/ConfigureRequestDispatching
//bridge - same reasoning as WdfDeviceCreate above: a COM-activated driver only ever reaches queue
//creation/dispatch-registration through these two flat functions called directly, never through
//its own flat WDFFUNC table (which it never gets initialized for).
struct winwdf_queue;
__winfnc NTSTATUS WdfIoQueueCreate(WDF_DRIVER_GLOBALS *globals, WDFOBJECT device, WDF_IO_QUEUE_CONFIG *config, WDF_OBJECT_ATTRIBUTES *obj_attrs, WDFOBJECT *out);
__winfnc NTSTATUS WdfDeviceConfigureRequestDispatching(WDF_DRIVER_GLOBALS *globals, WDFOBJECT device, WDFOBJECT queue_obj, WDF_REQUEST_TYPE req_type);
//Bridge-context accessors (see struct winwdf_queue's com_devctrl_callback/com_facade fields).
void wdf_queue_set_com_bridge(struct winwdf_queue *queue, void *devctrl_callback, void *com_facade);
void *wdf_queue_get_com_devctrl_callback(struct winwdf_queue *queue);
void *wdf_queue_get_com_facade(struct winwdf_queue *queue);

#endif