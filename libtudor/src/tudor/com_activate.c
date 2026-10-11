#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "winapi/wdf/internal.h" //struct wdf_memory, wdf_usb_device/interface/pipe accessors - see the USB target bridge below

//Bridges into drivers using the legacy UMDF 1.x COM-based activation model (DllGetClassObject ->
//IClassFactory -> IDriverEntry::OnInitialize/OnDeviceAdd), as opposed to the modern UMDF2/KMDF-
//compatible flat WDF_FUNCTION_TABLE model the rest of this project (winapi/wdf/*.c) already
//supports and which FxDriverEntryUm-exporting drivers use directly.
//
//Rather than hand-deriving the exact vtable layout of IWDFDriver/IWDFDeviceInitialize (large,
//loosely-documented COM interfaces) upfront, every vtable slot beyond the standard IUnknown ones
//starts as a generic stub that logs its own slot index and a few raw argument values, then
//returns a generic success value. Real driver behavior determines the actual call sequence live,
//and each slot gets replaced with a real implementation - typically delegating to the equivalent
//already-implemented flat-table function in winapi/wdf/device.c - once its identity is confirmed.
//
//Activation happens in two phases, mirroring how OnInitialize/OnDeviceAdd map onto real PnP
//timing: OnInitialize (tudor_init_com_driver) runs once during tudor_init(), before any USB
//device handle is available; OnDeviceAdd (tudor_com_add_device) - which is what actually calls
//CreateDevice - is deferred until a device is actually being opened, same as the flat-table path's
//winwdf_add_device.

#define COM_VTABLE_SLOTS 48

//Real WDK IIDs (wudfddi.h) - used only to tag our own facades for QueryInterface identity checks
//(see struct com_obj::iface_tag / com_query_interface below), not for matching against what the
//driver actually requests. IWDFDevice has versioned siblings (IWDFDevice2/3, each inheriting the
//last and only ever *appending* methods - standard COM practice for binary compatibility, so
//slots 0-24 are identical across all three) that this driver was observed actually requesting
//instead of plain IWDFDevice; IWDFDriver has no such siblings.
static const GUID g_iid_iwdf_driver = DEFINE_GUID(ad368ebe, 4139, 43e3, a875, 69b266a9139c);
static const GUID g_iid_iwdf_device = DEFINE_GUID(d657fe45, 460a, 49c3, 8219, 766ae8032a80);
static const GUID g_iid_iwdf_device2 = DEFINE_GUID(1b0d07b1, f596, 4a76, 99b3, b9492ad8df9f);
static const GUID g_iid_iwdf_device3 = DEFINE_GUID(863d943a, c9cd, 4655, a8d6, c84ef411cc0d);
//IPnpCallbackHardware - identified live by disassembling synaWudfBioUsb52.dll: the "obj_attrs"
//pointer IWDFDriver::CreateDevice receives as its 3rd argument (see com_create_device below)
//resolved, via debug string literals sitting next to its vtable in the driver's .rdata, to
//"CBiometricDeviceUSB::OnPrepareHardware"/"...::OnReleaseHardware" - i.e. it's not a
//WDF_OBJECT_ATTRIBUTES* at all (despite matching that parameter slot in the flat-table
//WdfDeviceCreate signature) but the driver's own pCallbackInterface object, which real UMDF hosts
//are expected to QueryInterface for IPnpCallbackHardware/IPnpCallback and invoke as part of the PnP
//AddDevice sequence - OnPrepareHardware first (once OnDeviceAdd itself has fully returned, see
//tudor_com_add_device), then IPnpCallback::OnD0Entry (the power-up transition that real WDF always
//runs after hardware prep, before the device is usable). A CreateThread call was found statically
//inside CBiometricDeviceUSB::OnPrepareHardware gated behind some device-context flags, originally
//assumed to be what clears the sensor's permanent post-construction WINBIO_SENSOR_BUSY status - live
//breakpoint tracing disproved this (CreateThread is never actually reached from OnPrepareHardware in
//practice), so OnD0Entry was added as the next real PnP-lifecycle step to try.
static const GUID g_iid_ipnp_callback_hardware = DEFINE_GUID(51433bd3, c7c1, 4bd8, b4c1, ab1e034626cc);
static const GUID g_iid_ipnp_callback = DEFINE_GUID(27c32374, cc45, 4840, 857e, 8e5ef7c0ebff);
//IWDFUsbTargetFactory - the documented UMDF 1.x mechanism for USB access (QueryInterface on the
//device object, then CreateUsbTargetDevice). Confirmed live this driver never actually requests
//it (see FINDINGS.md) - wired up here
//anyway on the chance some as-yet-unfound precondition is what's actually gating the attempt, not
//the interface's absence. Real vtable layouts below verified directly against wudfusb.h's raw
//flattened C-style vtable structs (not a summarized/renumbered fetch, whose numbering
//can be off by one).
static const GUID g_iid_iwdf_usb_target_factory = DEFINE_GUID(3f7becf9, 3a65, 4348, a4f3, 339d5734a9c6);

//IWDFPropertyStoreFactory - found live tracing the exact same "WinUsb_Initialize succeeds, then a
//crash right after" chain triggered by CreateFileA also recognizing OnPrepareHardware's own device
//name (see FINDINGS.md): the driver QueryInterface's device_obj for an IID this project didn't
//recognize, got a blank generic-stub alias back, then called its slot 3 expecting an out-param to
//be populated - the exact same "untouched out-param" bug class as RetrieveDeviceName/
//RetrieveDeviceInstanceId/RetrieveDevicePropertyStore above, just on a different, previously-
//unreached interface. Identifying the actual GUID took a second, independent discovery: this
//project's own QueryInterface-miss debug log prints the raw in-memory GUID bytes directly
//(`b[0..15]`) rather than correcting for x86's little-endian storage of GUID PartA/PartB/PartC
//(unlike PartD/PartE, which DEFINE_GUID already byte-swaps to store correctly - see windows.h) -
//a real, previously-undiscovered bug in the logging itself, not the comparison logic (guid_eq's
//memcmp is unaffected). The logged "067ebe45-659b-4d43-a7d6-9572d7f73d53" had to be re-derived by
//reversing PartA (4 bytes) and PartB/PartC (2 bytes each) back to canonical order before it matched
//anything in a real WDK header (wudfddi.h, IID_IWDFPropertyStoreFactory) - Data4 (the last 8 bytes)
//needed no correction, which is what made the mismatch subtle rather than obviously wrong-looking.
static const GUID g_iid_iwdf_property_store_factory = DEFINE_GUID(45be7e06, 9b65, 434d, a7d6, 9572d7f73d53);

enum com_iface_tag { COM_IFACE_NONE = 0, COM_IFACE_IWDFDRIVER, COM_IFACE_IWDFDEVICE };

static struct com_obj *get_usb_target_factory_facade(struct winwdf_device *device);
static struct com_obj *get_property_store_factory_facade(void);

typedef void *(__winfnc *com_generic_fnc)(void *self, void *a, void *a2, void *a3, void *a4, void *a5);

struct com_obj {
    void **vtable;
    void *user_data; //Points at whatever real WDF struct this facade wraps (winwdf_driver, wdf_device_init, etc.)
    enum com_iface_tag iface_tag; //COM_IFACE_NONE for generic/untagged facades; set only on objects
                                   //with real per-interface vtable overrides (g_wdf_driver_obj,
                                   //device_obj), so QueryInterface can tell "the driver re-querying
                                   //(an IID compatible with) the interface it already has" apart from
                                   //"the driver querying a genuinely different interface" (see
                                   //com_query_interface).
};

static void make_com_obj(struct com_obj *obj, void *user_data);

static bool guid_eq(const GUID *a, const GUID *b) { return a && b && memcmp(a, b, sizeof(GUID)) == 0; }

static bool iid_matches_tag(enum com_iface_tag tag, const GUID *iid) {
    switch(tag) {
        case COM_IFACE_IWDFDRIVER: return guid_eq(iid, &g_iid_iwdf_driver);
        case COM_IFACE_IWDFDEVICE: return guid_eq(iid, &g_iid_iwdf_device) || guid_eq(iid, &g_iid_iwdf_device2) || guid_eq(iid, &g_iid_iwdf_device3);
        default: return false;
    }
}

//Returning 'self' here unconditionally (regardless of the requested IID) is unsound whenever self's
//vtable carries slot overrides for a *specific* interface (e.g. g_wdf_driver_obj's IWDFDriver::
//CreateDevice at slot 8): a QueryInterface for some *other*, unrelated interface on the same object
//legitimately gets back the same override at whatever slot that other interface happens to use, and
//a stray call there silently misfires into our real implementation with garbage arguments instead of
//hitting a harmless logging stub.
//
//But always handing back a fresh, override-free facade regardless of IID is *also* wrong: drivers
//routinely re-QueryInterface an object for a compatible interface they already have (e.g. device_obj
//being asked for IID_IWDFDevice3 when it already represents IWDFDevice's method layout), and that
//legitimately-compatible request was losing device_obj's own CreateIoQueue/ConfigureRequestDispatching
//overrides to a blank alias, silently breaking devctrl entirely. Tagging objects that carry real
//overrides with the interface family they represent (see g_wdf_driver_obj/device_obj below) lets
//QueryInterface tell the two cases apart: a compatible IID returns self (keeping its overrides),
//anything else still gets a fresh, override-free alias.
__winfnc static void *com_query_interface(struct com_obj *self, const GUID *iid, void **out) {
    if(iid && iid_matches_tag(self->iface_tag, iid)) {
        *out = self;
        return 0; //S_OK
    }

    //IWDFUsbTargetFactory is only ever obtained via QueryInterface on the device object itself
    //(never a re-query of an existing facade), so this doesn't need iface-family tagging like
    //IWDFDevice/IWDFDriver above - just a direct self-identity check.
    if(iid && self->iface_tag == COM_IFACE_IWDFDEVICE && guid_eq(iid, &g_iid_iwdf_usb_target_factory)) {
        *out = get_usb_target_factory_facade((struct winwdf_device*) self->user_data);
        return 0; //S_OK
    }

    if(iid && self->iface_tag == COM_IFACE_IWDFDEVICE && guid_eq(iid, &g_iid_iwdf_property_store_factory)) {
        *out = get_property_store_factory_facade();
        return 0; //S_OK
    }

    if(iid) {
        const uint8_t *b = (const uint8_t*) iid;
        log_debug("QueryInterface miss (self=%p tag=%d) for IID %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            self, self->iface_tag,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }

    struct com_obj *alias = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(alias, self->user_data);
    *out = alias;
    return 0; //S_OK
}
__winfnc static void *com_add_ref(struct com_obj *self) { return (void*) 1; }
__winfnc static void *com_release(struct com_obj *self) { return (void*) 1; }

static const char *g_stub_iface_name;

//g_stub_iface_name is only a rough hint (whatever interface we most recently *handed to the
//driver*, e.g. "IWDFDeviceInitialize" during OnDeviceAdd) - it does NOT track which object a given
//call actually landed on. A driver that already holds a persistent object (device_obj,
//g_wdf_driver_obj) can call straight into it without ever re-QueryInterface-ing, so a slot number
//alone can still land on a *different* real interface than the label suggests (e.g. slots #15/#16
//during OnDeviceAdd turned out to be IWDFDevice::CreateDeviceInterface/AssignDeviceInterfaceState
//on the freshly-minted device_obj, not anything on IWDFDeviceInitialize, whose vtable doesn't even
//have 15 slots). Cross-check self against known persistent objects (device_obj, g_wdf_driver_obj)
//and the real WDK vtable layout before trusting this label.
//This generic stub returns S_OK (0) unconditionally, which is fine for the many void/no-out-param
//methods that land here (SetLockingConstraint, SetPowerPolicyOwnership, etc.) and even for some
//with out-params the driver doesn't treat as fatal on failure (CreateDeviceInterface/
//AssignDeviceInterfaceState - tried making this stub return E_NOTIMPL uniformly at one point since
//IWDFDevice3::MapIoSpace/CreateInterrupt crash when their out-param is left unset after a "success"
//return, but that broke OnDeviceAdd entirely: it turns out the driver DOES treat
//CreateDeviceInterface's failure as fatal, aborting device add with the propagated E_NOTIMPL. So
//S_OK stays the default here, and methods that are unsafe to no-op this way (MapIoSpace) get their
//own explicit, real override instead (see device_obj's vtable setup in com_create_device) rather
//than changing this shared default.
static void *com_generic_stub_impl(int slot, void *self, void *a, void *a2, void *a3, void *a4, void *a5) {
    log_debug("Unimplemented %s-ish COM vtable slot #%d called! [self=%p args: %p %p %p %p %p]", g_stub_iface_name, slot, self, a, a2, a3, a4, a5);
    return 0; //S_OK
}

//Generates COM_VTABLE_SLOTS distinct __winfnc trampolines (one per slot index), each of which just
//forwards to com_generic_stub_impl with its own slot number baked in - a stand-in for real
//per-interface method implementations that get filled in individually as they're identified.
#define STUB(n) __winfnc static void *com_stub_##n(void *self, void *a, void *a2, void *a3, void *a4, void *a5) { return com_generic_stub_impl(n, self, a, a2, a3, a4, a5); }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9)
STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18) STUB(19)
STUB(20) STUB(21) STUB(22) STUB(23) STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29)
STUB(30) STUB(31) STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39)
STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47)
#undef STUB

static void *generic_stub_table[COM_VTABLE_SLOTS] = {
    com_stub_0, com_stub_1, com_stub_2, com_stub_3, com_stub_4, com_stub_5, com_stub_6, com_stub_7, com_stub_8, com_stub_9,
    com_stub_10, com_stub_11, com_stub_12, com_stub_13, com_stub_14, com_stub_15, com_stub_16, com_stub_17, com_stub_18, com_stub_19,
    com_stub_20, com_stub_21, com_stub_22, com_stub_23, com_stub_24, com_stub_25, com_stub_26, com_stub_27, com_stub_28, com_stub_29,
    com_stub_30, com_stub_31, com_stub_32, com_stub_33, com_stub_34, com_stub_35, com_stub_36, com_stub_37, com_stub_38, com_stub_39,
    com_stub_40, com_stub_41, com_stub_42, com_stub_43, com_stub_44, com_stub_45, com_stub_46, com_stub_47,
};

static void make_com_obj(struct com_obj *obj, void *user_data) {
    void **vtable = (void**) malloc(COM_VTABLE_SLOTS * sizeof(void*));
    vtable[0] = com_query_interface;
    vtable[1] = com_add_ref;
    vtable[2] = com_release;
    memcpy(vtable + 3, generic_stub_table + 3, (COM_VTABLE_SLOTS - 3) * sizeof(void*));
    obj->vtable = vtable;
    obj->user_data = user_data;
    obj->iface_tag = COM_IFACE_NONE;
}

//IWDFDriver::CreateDevice - identified live: called with (DeviceInit, Attributes, &out_Device),
//where DeviceInit is the very same IWDFDeviceInitialize COM object handed to the driver via
//OnDeviceAdd (drivers treat it as an opaque token to hand back, not something they inspect
//directly), delegates to the already-implemented flat-table WdfDeviceCreate.
//
//The driver expects *out to be a real IWDFDevice* (starting with a vtable, like any COM object),
//not the raw internal WDFOBJECT WdfDeviceCreate returns - the driver was observed immediately
//calling QueryInterface (vtable slot 0) through it, which crashed dereferencing our internal
//struct's first field as if it were a vtable pointer. So the raw WDFOBJECT gets wrapped in its
//own persistent COM facade (the driver holds onto this for its own lifetime, unlike the
//transient IWDFDeviceInitialize) before being handed back.
//
//obj_attrs here is NOT a WDF_OBJECT_ATTRIBUTES* in the usual sense - it's actually a genuine COM
//interface pointer the driver obtained via its own QueryInterface on the device-init chain (offset
//0x2e0 into a driver-internal, real, refcounted object), reused as this call's third argument. The
//driver's call site unconditionally releases its own reference to it right after this call returns,
//trusting that a real handler AddRef's its own copy first if it needs the object to stay alive past
//that point - which is exactly what's needed here, since this same object is what tudor_open() later
//re-derefs, and skipping the AddRef let that unconditional Release drop the shared refcount to zero
//and free it out from under the caller.
typedef void *__winfnc (*com_raw_addref_fnc)(void *self);
static void com_raw_add_ref(void *raw_com_ptr) {
    void **vtable = *(void***) raw_com_ptr;
    ((com_raw_addref_fnc) vtable[1])(raw_com_ptr);
}

typedef void *__winfnc (*com_raw_qi_fnc)(void *self, const GUID *iid, void **out);

//IWDFMemory - just enough to bridge a devctrl request's in/out buffers (which our own request
//machinery already tracks via winwdf_get_request_info) into something the driver's real
//IQueueCallbackDeviceIoControl callback can call GetDataBuffer/CopyToBuffer/CopyFromBuffer/GetSize
//on, rather than a genuine WDF memory object.
struct com_memory_data {
    void *buf;
    size_t size;
};

__winfnc static void *com_memory_copy_to_buffer(struct com_obj *self, ULONG_PTR src_off, void *dst, size_t num) {
    struct com_memory_data *mem = (struct com_memory_data*) self->user_data;
    if(src_off + num > mem->size) return (void*) (intptr_t) 0x8007007A; /* E_INSUFFICIENT_BUFFER-ish */
    memcpy(dst, (char*) mem->buf + src_off, num);
    return 0;
}
__winfnc static void *com_memory_copy_from_buffer(struct com_obj *self, ULONG_PTR dst_off, void *src, size_t num) {
    struct com_memory_data *mem = (struct com_memory_data*) self->user_data;
    if(dst_off + num > mem->size) return (void*) (intptr_t) 0x8007007A;
    memcpy((char*) mem->buf + dst_off, src, num);
    return 0;
}
__winfnc static size_t com_memory_get_size(struct com_obj *self) {
    return ((struct com_memory_data*) self->user_data)->size;
}
__winfnc static void *com_memory_get_data_buffer(struct com_obj *self, size_t *out_size) {
    struct com_memory_data *mem = (struct com_memory_data*) self->user_data;
    if(out_size) *out_size = mem->size;
    return mem->buf;
}

static struct com_obj *make_com_memory_obj(void *buf, size_t size) {
    struct com_memory_data *mem = (struct com_memory_data*) malloc(sizeof(struct com_memory_data));
    *mem = (struct com_memory_data) { .buf = buf, .size = size };

    struct com_obj *obj = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(obj, mem);
    obj->vtable[9] = com_memory_copy_to_buffer;    //IWDFMemory::CopyToBuffer
    obj->vtable[10] = com_memory_copy_from_buffer; //IWDFMemory::CopyFromBuffer
    obj->vtable[11] = com_memory_get_size;         //IWDFMemory::GetSize
    obj->vtable[12] = com_memory_get_data_buffer;  //IWDFMemory::GetDataBuffer
    return obj;
}

//IWDFIoRequest - only the handful of methods a device-control handler actually needs (see
//com_queue_evt_io_device_control below); every other slot stays generically stubbed, which is fine
//since this facade only ever lives for the duration of a single devctrl callback, never stored by
//the driver past that call. user_data points at a stack-local com_request_data, not a struct
//com_obj-wrapped winwdf_request directly, since the driver expects real IN/OUT-then-CODE ordered
//devctrl parameters back out of GetDeviceIoControlParameters, and our own winwdf_request has no
//field to remember the IoControlCode separately from the (differently-ordered) callback arguments
//that first delivered it.
struct com_request_data {
    struct winwdf_request *req;
    ULONG code;
    size_t in_size, out_size;

    struct com_obj *facade;
    void *cancel_cb; //IRequestCallbackCancel* registered through MarkCancelable
    bool cancelled;
};

//IWDFIoRequest::MarkCancelable/UnmarkCancelable (slots 19/20, verified against UMDF 1.11's
//wudfddi.h). The driver marks its StartCapture request cancelable with its own
//IRequestCallbackCancel and only aborts the pending finger wait (WinUsb_AbortPipe on the interrupt
//pipe) from OnCancel - without this, cancelling a capture never reached the driver. Bridged onto the
//flat WdfRequestMarkCancelable; WDF requests get reused across devctrls, so the trampoline finds
//the current facade through this small req -> request data map.
struct com_cancel_entry { struct com_cancel_entry *next; struct com_request_data *rd; };
static pthread_mutex_t g_cancel_lock = PTHREAD_MUTEX_INITIALIZER;
static struct com_cancel_entry *g_cancel_head = NULL;

typedef void __winfnc (*com_oncancel_fnc)(void *self, void *wdf_request);

__winfnc static void com_request_cancel_trampoline(WDFOBJECT req_obj) {
    struct com_request_data *rd = NULL;
    cant_fail_ret(pthread_mutex_lock(&g_cancel_lock));
    for(struct com_cancel_entry *e = g_cancel_head; e; e = e->next) {
        if(e->rd->req == (struct winwdf_request*) req_obj) { rd = e->rd; break; }
    }
    void *cb = rd ? rd->cancel_cb : NULL;
    if(rd) rd->cancelled = true;
    cant_fail_ret(pthread_mutex_unlock(&g_cancel_lock));

    log_verbose("devctrl request %p cancelled, driver cancel callback %p", req_obj, cb);
    if(cb) ((com_oncancel_fnc) (*(void***) cb)[3])(cb, rd->facade);
}

static void com_cancel_map_set(struct com_request_data *rd) {
    cant_fail_ret(pthread_mutex_lock(&g_cancel_lock));
    struct com_cancel_entry *e;
    for(e = g_cancel_head; e; e = e->next) if(e->rd->req == rd->req) break;
    if(!e) {
        e = (struct com_cancel_entry*) malloc(sizeof(struct com_cancel_entry));
        if(!e) { perror("Couldn't allocate cancel map entry"); abort(); }
        e->next = g_cancel_head;
        g_cancel_head = e;
    }
    e->rd = rd;
    cant_fail_ret(pthread_mutex_unlock(&g_cancel_lock));
}

__winfnc static HRESULT com_request_mark_cancelable(struct com_obj *self, void *cancel_cb) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    log_verbose("com_request_mark_cancelable self=%p req=%p cb=%p code=0x%x", self, rd->req, cancel_cb, rd->code);
    cant_fail_ret(pthread_mutex_lock(&g_cancel_lock));
    rd->cancel_cb = cancel_cb;
    cant_fail_ret(pthread_mutex_unlock(&g_cancel_lock));
    com_cancel_map_set(rd);
    WdfRequestMarkCancelable(NULL, (WDFOBJECT) rd->req, com_request_cancel_trampoline);
    return 0; //S_OK
}

__winfnc static HRESULT com_request_unmark_cancelable(struct com_obj *self) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    WdfRequestUnmarkCancelable(NULL, (WDFOBJECT) rd->req);
    cant_fail_ret(pthread_mutex_lock(&g_cancel_lock));
    rd->cancel_cb = NULL;
    bool cancelled = rd->cancelled;
    cant_fail_ret(pthread_mutex_unlock(&g_cancel_lock));
    log_verbose("com_request_unmark_cancelable self=%p req=%p cancelled=%d", self, rd->req, cancelled);
    //Real UMDF: HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) if OnCancel already ran / is running
    return cancelled ? (HRESULT) 0x800703E3 : 0;
}

__winfnc static void *com_request_get_devctrl_params(struct com_obj *self, ULONG *out_code, size_t *out_in_size, size_t *out_out_size) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    if(out_code) *out_code = rd->code;
    if(out_in_size) *out_in_size = rd->in_size;
    if(out_out_size) *out_out_size = rd->out_size;
    return 0;
}
//Confirmed live: the real driver's own DeviceIoControl call for enrollment's devctrl (0x44200c) sets
//in_size=out_size=0 (accurately reflected here), yet its later code unconditionally copies a fixed
//number of bytes (0x20, observed live - traced to come from the INPUT side specifically) out of
//whatever GetInputMemory/GetDataBuffer returned, without checking that size first - the real driver's
//own inconsistency between what it requests via DeviceIoControl and what it internally assumes is
//available. A genuinely NULL/empty buffer here crashes that copy. Substituting a small zeroed scratch
//buffer whenever the real buffer is NULL gives that unconditional read somewhere safe to land, at the
//cost of the driver reading zeros instead of real, negotiated memory it never asked the I/O manager
//for in the first place. Applied to both GetInputMemory and GetOutputMemory defensively, since the
//same real-driver pattern could plausibly hit either side. See FINDINGS.md.
#define REQUEST_MEMORY_SCRATCH_SIZE 0x100
static struct com_obj *make_com_memory_obj_or_scratch(const void *buf, size_t size) {
    if(!buf) { buf = calloc(1, REQUEST_MEMORY_SCRATCH_SIZE); size = REQUEST_MEMORY_SCRATCH_SIZE; }
    return make_com_memory_obj((void*) buf, size);
}
__winfnc static void *com_request_get_output_memory(struct com_obj *self, struct com_obj **out) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    const void *out_buf; size_t out_size;
    winwdf_get_request_info(rd->req, NULL, NULL, NULL, &out_buf, &out_size, NULL);
    *out = make_com_memory_obj_or_scratch(out_buf, out_size);
    return 0;
}
__winfnc static void *com_request_get_input_memory(struct com_obj *self, struct com_obj **out) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    const void *in_buf; size_t in_size;
    winwdf_get_request_info(rd->req, NULL, &in_buf, &in_size, NULL, NULL, NULL);
    *out = make_com_memory_obj_or_scratch(in_buf, in_size);
    return 0;
}
__winfnc static void *com_request_set_information(struct com_obj *self, ULONG_PTR info) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    log_verbose("com_request_set_information self=%p req=%p info=0x%lx code=0x%x", self, rd->req, info, rd->code);
    WdfRequestSetInformation(NULL, (WDFOBJECT) rd->req, info);
    return 0;
}
__winfnc static void *com_request_complete(struct com_obj *self, HRESULT status) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    log_verbose("com_request_complete self=%p req=%p status=0x%x code=0x%x", self, rd->req, status, rd->code);
    WdfRequestComplete(NULL, (WDFOBJECT) rd->req, (NTSTATUS) status);
    return 0;
}
__winfnc static void *com_request_complete_with_information(struct com_obj *self, HRESULT status, size_t info) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    log_verbose("com_request_complete_with_information self=%p req=%p status=0x%x info=0x%zx code=0x%x", self, rd->req, status, info, rd->code);
    WdfRequestSetInformation(NULL, (WDFOBJECT) rd->req, info);
    WdfRequestComplete(NULL, (WDFOBJECT) rd->req, (NTSTATUS) status);
    return 0;
}
//IWDFIoRequest::Send (slot 23, verified against wudfddi.h's raw flattened vtable struct) - needed
//for the IWDFUsbTargetPipe bridge below: after FormatRequestForRead/Write configures a request
//against a pipe/device target, the driver calls Send on the SAME request (not a method on the
//target) to actually dispatch it. pIoTarget/Flags aren't used by this project's underlying
//wdf_start_request/usb_transfer_start machinery (the target to operate on was already captured at
//FormatRequest time), so they're accepted and ignored rather than needing a real IWDFIoTarget
//implementation.
__winfnc static HRESULT com_request_send(struct com_obj *self, void *io_target, ULONG flags, LONGLONG timeout) {
    struct com_request_data *rd = (struct com_request_data*) self->user_data;
    log_verbose("com_request_send self=%p req=%p io_target=%p flags=0x%x", self, rd->req, io_target, flags);
    wdf_start_request(rd->req, NULL, -1);
    return 0; //S_OK
}

typedef void __winfnc (*com_ondeviceiocontrol_fnc)(void *self, void *wdf_queue, void *wdf_request, ULONG code, size_t in_size, size_t out_size);

//Bridges a real WDF devctrl dispatch (winapi/wdf/queue.c's wdf_queue_devctrl, itself reached via
//tudor_devctrl in device.c) into the driver's own COM IQueueCallbackDeviceIoControl::
//OnDeviceIoControl, using the callback pointer and persistent queue facade com_create_io_queue
//stashed on the queue below. Note the parameter order flip: our internal EvtIoDeviceControl shape
//is (Queue, Request, OutLen, InLen, Code) but the real COM method takes IN before OUT.
__winfnc static void com_queue_evt_io_device_control(WDFOBJECT queue_obj, WDFOBJECT req_obj, size_t out_len, size_t in_len, ULONG code) {
    struct winwdf_queue *queue = (struct winwdf_queue*) queue_obj;
    void *driver_callback = wdf_queue_get_com_devctrl_callback(queue);
    struct com_obj *queue_facade = (struct com_obj*) wdf_queue_get_com_facade(queue);
    if(!driver_callback || !queue_facade) {
        log_warn("Devctrl request dispatched to a queue with no COM callback registered!");
        WdfRequestComplete(NULL, req_obj, 0xC0000001 /* STATUS_UNSUCCESSFUL */);
        return;
    }

    //Heap-allocated (NOT stack-local): a real devctrl handler is frequently async (e.g. StartCapture
    //waits for a finger touch), meaning it returns from OnDeviceIoControl below without calling
    //Complete/CompleteWithInformation, and instead stashes this IWDFIoRequest pointer somewhere to
    //call back into once the real result is ready - which may be long after this function's stack
    //frame is gone. A stack-local facade here was a silent dangling-pointer bug: the driver's later
    //completion attempt called through freed stack memory and never actually reached
    //WdfRequestComplete, which is why async devctrl requests (StartCapture) never completed except
    //via the watchdog's forced timeout.
    //
    //Deliberately never freed: com_add_ref/com_release (this file, above) are no-op stubs with no
    //real refcounting, so there's no reliable signal for "the driver is truly done with this
    //object." Freeing it from com_request_complete[_with_information] was tried and caused a real
    //use-after-free, observed live - the driver (at least for the Reset/QueryStatus completion
    //path) keeps calling through the same IWDFIoRequest pointer *after* Complete, presumably for
    //bookkeeping/logging on its own request object. Leaking one small facade per devctrl request is
    //a deliberate, low-cost tradeoff over a live crash.
    struct com_request_data *rd = (struct com_request_data*) malloc(sizeof(struct com_request_data));
    *rd = (struct com_request_data) { .req = (struct winwdf_request*) req_obj, .code = code, .in_size = in_len, .out_size = out_len };
    struct com_obj *req_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(req_facade, rd);
    rd->facade = req_facade;
    req_facade->vtable[8] = com_request_complete_with_information; //IWDFIoRequest::CompleteWithInformation
    req_facade->vtable[9] = com_request_set_information;           //IWDFIoRequest::SetInformation
    req_facade->vtable[10] = com_request_complete;                 //IWDFIoRequest::Complete
    req_facade->vtable[16] = com_request_get_devctrl_params;       //IWDFIoRequest::GetDeviceIoControlParameters
    req_facade->vtable[17] = com_request_get_output_memory;        //IWDFIoRequest::GetOutputMemory
    req_facade->vtable[18] = com_request_get_input_memory;         //IWDFIoRequest::GetInputMemory
    req_facade->vtable[19] = com_request_mark_cancelable;          //IWDFIoRequest::MarkCancelable
    req_facade->vtable[20] = com_request_unmark_cancelable;        //IWDFIoRequest::UnmarkCancelable
    req_facade->vtable[23] = com_request_send;                     //IWDFIoRequest::Send

    void **cb_vtable = *(void***) driver_callback;
    ((com_ondeviceiocontrol_fnc) cb_vtable[3])(driver_callback, queue_facade, req_facade, code, in_len, out_len);
}

//IWDFDevice::GetDefaultIoTarget - slot 11 per the same wudfddi.h vtable-counting as CreateIoQueue
//below (RetrieveDevicePropertyStore=8, GetDriver=9, RetrieveDeviceInstanceId=10,
//GetDefaultIoTarget=11). Never previously implemented, meaning any driver code calling this fell
//through to the generic stub, which leaves *ppWdfIoTarget unset (a real out-param, not a harmless
//void-returning hint like most of what lands on the generic stub) - a real caller would then be
//holding garbage instead of a usable IWDFIoTarget*. Returns a persistent, generic-stub-backed
//facade (same pattern as com_create_interrupt) so a caller at least gets a valid, safely-callable
//object instead of garbage; every method on it still just logs and returns S_OK until identified.
__winfnc static void com_get_default_io_target(struct com_obj *self, struct com_obj **out) {
    log_verbose("com_get_default_io_target ENTER self=%p", self);
    static struct com_obj *io_target_facade = NULL;
    if(!io_target_facade) {
        io_target_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
        make_com_obj(io_target_facade, NULL);
    }
    *out = io_target_facade;
}

//IWDFNamedPropertyStore::GetNamedValue (slot 3 - QueryInterface=0, AddRef=1, Release=2 - per the
//real wudfddi.h vtable). Backing object for IWDFDevice::RetrieveDevicePropertyStore below.
//
//Found live while tracing the "PairingContext" singleton constructor's crash (see FINDINGS.md, COM
//bridge): the driver
//really does call GetNamedValue("PairingContext", &pv) as part of deciding whether a stored
//pairing context already exists. This project's generic stub answers S_OK without ever touching
//the caller's PROPVARIANT*, which - critically - is a *second*, independent instance of the same
//bug class as RetrieveDeviceName/RetrieveDeviceInstanceId above: an untouched out-param the driver
//trusts as valid. Always answering "not found" (VT_EMPTY, a real, well-formed empty PROPVARIANT)
//is the correct behavior for a fresh/never-paired device (matches this session's own external
//research - real hardware reports "not initialized" until a real pairing handshake completes) -
//this is deliberately not wired to this project's own pairing-data persistence (get_pair_data),
//since that store's key space and this property store's are not confirmed to be the same thing,
//and a wrong non-empty answer here risks steering the driver down a "reuse an existing pairing"
//path with data it can't actually validate.
struct propvariant { uint16_t vt, wReserved1, wReserved2, wReserved3; union { uint64_t val; void *ptr; } u; };
#define VT_EMPTY 0

__winfnc static HRESULT com_propstore_get_named_value(struct com_obj *self, const char16_t *name, struct propvariant *pv) {
    char *cname = name ? winstr_to_str(name) : NULL;
    log_verbose("com_propstore_get_named_value self=%p name='%s' -> not found (VT_EMPTY)", self, cname ? cname : "(null)");
    free(cname);
    if(!pv) return (HRESULT) 0x80070057; /* E_INVALIDARG */
    *pv = (struct propvariant) {0}; /* VT_EMPTY */
    return 0; /* S_OK */
}

//IWDFDevice::RetrieveDevicePropertyStore (slot 8, same wudfddi.h vtable-counting as the rest of
//this interface). The generic stub this landed on before returned S_OK without ever setting
//*ppPropStore - the real, load-bearing out-param - leaving the driver holding an uninitialized
//pointer it trusts is a valid IWDFNamedPropertyStore*, then calling through it
//(GetNamedValue("PairingContext", ...), live-confirmed - see FINDINGS.md). Returns a persistent,
//real facade (same pattern as com_get_default_io_target) so that call lands on real code instead
//of undefined behavior over garbage memory.
__winfnc static HRESULT com_retrieve_device_property_store(struct com_obj *self, const char16_t *service_name, ULONG flags, struct com_obj **out_store, ULONG *out_disposition) {
    log_verbose("com_retrieve_device_property_store ENTER self=%p flags=0x%lx", self, (unsigned long) flags);
    static struct com_obj *prop_store_facade = NULL;
    if(!prop_store_facade) {
        prop_store_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
        make_com_obj(prop_store_facade, NULL);
        prop_store_facade->vtable[3] = com_propstore_get_named_value; //IWDFNamedPropertyStore::GetNamedValue
    }
    if(!out_store) return (HRESULT) 0x80070057; /* E_INVALIDARG */
    *out_store = prop_store_facade;
    if(out_disposition) *out_disposition = 1; /* WdfPropertyStoreCreatedNew */
    return 0; /* S_OK */
}

//IWDFPropertyStoreFactory::RetrieveDevicePropertyStore (slot 3 - QueryInterface=0, AddRef=1,
//Release=2, per wudfddi.h's real IWDFPropertyStoreFactory vtable). Real signature: (RootSpecifier,
//Flags, DesiredAccess, SubkeyPath, **PropertyStore, *Disposition) - confirmed live matching exactly
//(rdx/r8/r9/stack args), landing on the generic stub before this fix, which left **PropertyStore
//(the real, load-bearing out-param) untouched - see g_iid_iwdf_property_store_factory's own comment
//for the full discovery story. Same "not found"/empty-property-store answer as
//com_retrieve_device_property_store above, via the same GetNamedValue backing - this factory
//interface just offers a way to ask for property stores other than the device's own default one
//(a specific subkey/root), which this project doesn't distinguish since every query answers
//VT_EMPTY regardless of which store it's nominally for.
__winfnc static HRESULT com_property_store_factory_retrieve(struct com_obj *self, void *root_specifier, ULONG flags, ULONG desired_access, const char16_t *subkey_path, struct com_obj **out_store, ULONG *out_disposition) {
    log_verbose("com_property_store_factory_retrieve ENTER self=%p flags=0x%lx access=0x%lx", self, (unsigned long) flags, (unsigned long) desired_access);
    static struct com_obj *prop_store_factory_facade = NULL;
    if(!prop_store_factory_facade) {
        prop_store_factory_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
        make_com_obj(prop_store_factory_facade, NULL);
        prop_store_factory_facade->vtable[3] = com_propstore_get_named_value; //IWDFNamedPropertyStore(2)::GetNamedValue
    }
    if(!out_store) return (HRESULT) 0x80070057; /* E_INVALIDARG */
    *out_store = prop_store_factory_facade;
    if(out_disposition) *out_disposition = 1; /* WdfPropertyStoreCreatedNew */
    return 0; /* S_OK */
}

static struct com_obj *get_property_store_factory_facade(void) {
    static struct com_obj *factory_facade = NULL;
    if(!factory_facade) {
        factory_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
        make_com_obj(factory_facade, NULL);
        factory_facade->vtable[3] = com_property_store_factory_retrieve; //IWDFPropertyStoreFactory::RetrieveDevicePropertyStore
    }
    return factory_facade;
}

//IWDFDevice::RetrieveDeviceInstanceId (slot 10, same wudfddi.h vtable-counting as the rest of this
//interface). Found live tracing the "PairingContext" singleton constructor's own gate check (see
//FINDINGS.md, COM bridge): it calls a generic
//wildcard-match helper against the literal pattern `"usb"`, comparing it against a field of a
//local struct this project has never populated - device_obj's own vtable slot 10 was still the
//generic stub, so any driver code calling RetrieveDeviceInstanceId got an empty string back,
//which can never match "usb", silently steering the driver down its non-USB
//(vtable-population-skipped) construction path instead of the real one. Two-phase pattern like
//RetrieveDeviceName above. Uses a plausible real device instance ID matching this exact
//hardware's actual USB IDs (confirmed via this session's external research - a real `lsusb -v`
//dump against genuine `06cb:0082` hardware, posted publicly against this exact PID).
#define FAKE_WDF_DEVICE_INSTANCE_ID "USB\\VID_06CB&PID_0082\\6&1a2b3c4d&0&1"
__winfnc static HRESULT com_retrieve_device_instance_id(struct com_obj *self, char16_t *out_id, DWORD *out_len) {
    static char16_t *wid = NULL;
    if(!wid) wid = winstr_from_str(FAKE_WDF_DEVICE_INSTANCE_ID);
    int full_len = winstr_len(wid) + 1;

    log_verbose("com_retrieve_device_instance_id ENTER self=%p out_id=%p *out_len=%u", self, out_id, out_len ? *out_len : 0);
    if(!out_len) return (HRESULT) 0x80070057; /* E_INVALIDARG */

    if(!out_id) {
        *out_len = (DWORD) full_len;
        return 0; /* S_OK */
    }

    if((int) *out_len < full_len) {
        *out_len = (DWORD) full_len;
        return (HRESULT) 0x8007007a; /* ERROR_INSUFFICIENT_BUFFER */
    }

    memcpy(out_id, wid, full_len * sizeof(char16_t));
    *out_len = (DWORD) full_len;
    return 0; /* S_OK */
}

//IWDFDevice::RetrieveDeviceName (slot 17, same wudfddi.h vtable-counting as GetDefaultIoTarget/
//CreateIoQueue below). This is the real, concrete gate this whole investigation was looking for
//(see FINDINGS.md, "Activation and device setup"): the driver's real IPnpCallbackHardware::
//OnPrepareHardware implementation calls this with pDeviceName=NULL as the standard Win32
//"query the required buffer size first" pattern, and treats a *zero* returned length as "no name
//available" - silently skipping all of its real hardware-initialization work (which a captured
//real Windows USB trace confirms includes a genuine pairing/provisioning handshake with the
//sensor) rather than failing loudly. The generic stub answered S_OK without ever touching
//pdwDeviceNameLength, which stayed at the caller's own zero-initialized value - not a crash, just
//a silently-taken "nothing to do here" branch.
//
//CONTENT MATTERS, not just nonzero length: live-traced (see FINDINGS.md, "Activation and device
//setup") that this exact string, after being converted to
//ANSI (WideCharToMultiByte) a few instructions later in the driver's own OnPrepareHardware body,
//is directly wildcard-matched against the literal pattern "usb" - and the match result gates
//whether a device-scoped singleton object's vtable gets populated at all (its "PairingContext"
//teardown otherwise crashes on a still-null destructor slot - see FINDINGS.md).
//The original placeholder name didn't contain "usb", so the match failed every time. Must
//contain "usb" (lowercase - the pattern is a literal lowercase string, not confirmed
//case-insensitive) somewhere in the string.
#define FAKE_WDF_DEVICE_NAME "\\Device\\usbpdo-0"
__winfnc static HRESULT com_retrieve_device_name(struct com_obj *self, char16_t *out_name, DWORD *out_len) {
    static char16_t *wname = NULL;
    if(!wname) wname = winstr_from_str(FAKE_WDF_DEVICE_NAME);
    int full_len = winstr_len(wname) + 1;

    log_verbose("com_retrieve_device_name ENTER self=%p out_name=%p *out_len=%u", self, out_name, out_len ? *out_len : 0);
    if(!out_len) return (HRESULT) 0x80070057; /* E_INVALIDARG */

    if(!out_name) {
        *out_len = (DWORD) full_len;
        return 0; /* S_OK */
    }

    if((int) *out_len < full_len) {
        *out_len = (DWORD) full_len;
        return (HRESULT) 0x8007007a; /* ERROR_INSUFFICIENT_BUFFER */
    }

    memcpy(out_name, wname, full_len * sizeof(char16_t));
    *out_len = (DWORD) full_len;
    return 0; /* S_OK */
}

//IWDFDevice::CreateIoQueue - identified live by cross-referencing the real WDK's wudfddi.h: counting
//vtable slots (IWDFObject's 8 IUnknown+base slots, then IWDFDevice's own methods in declaration
//order - RetrieveDevicePropertyStore, GetDriver, RetrieveDeviceInstanceId, GetDefaultIoTarget,
//CreateWdfFile, GetDefaultIoQueue) lands CreateIoQueue exactly on slot 14, matching the arguments
//observed live: (pCallbackInterface, bDefaultQueue, DispatchType, bPowerManaged,
//bAllowZeroLengthRequests, ppIoQueue). Without a real implementation here, the driver's devctrl
//queue never gets created, so IWDFDevice::ConfigureRequestDispatching below has nothing to register
//and every subsequent device-control IOCTL (e.g. the sensor's Reset()) fails outright. Delegates to
//the existing flat-table WdfIoQueueCreate, stashing the driver's own COM devctrl callback on the
//resulting queue for com_queue_evt_io_device_control to use once real requests start arriving.
//IWDFDevice3::MapIoSpace - explicit override (rather than relying on the shared generic-stub
//default) because this one has a real out-parameter (**pPseudoBaseAddress) that a "success" return
//promises is valid; a plain USB device never has real MMIO to map, so this fails outright rather
//than the generic stub's blanket S_OK, which the driver would otherwise take as license to
//dereference the (unset) pseudo-address and crash.
__winfnc static HRESULT com_map_io_space(struct com_obj *self, uint64_t phys_addr, size_t num_bytes, ULONG cache_type, void **out) {
    return (HRESULT) 0x80004001; /* E_NOTIMPL */
}

__winfnc static void *com_create_io_queue(struct com_obj *self, void *callback_iface, BOOL default_queue, ULONG dispatch_type, BOOL power_managed, BOOL allow_zero_len, struct com_obj **out) {
    struct winwdf_device *device = (struct winwdf_device*) self->user_data;

    //QueryInterface the driver's own callback object for IQueueCallbackDeviceIoControl - a real
    //call on the driver's own vtable (not one of ours), so no IID-matching concerns here.
    void *devctrl_callback = NULL;
    if(callback_iface) {
        const GUID iid_devctrl_cb = DEFINE_GUID(c5411408, 0f1e, 4ed6, a412, 36dd15eee707);
        void **cb_vtable = *(void***) callback_iface;
        void *qi_result = ((com_raw_qi_fnc) cb_vtable[0])(callback_iface, &iid_devctrl_cb, &devctrl_callback);
        log_verbose("com_create_io_queue QI for IQueueCallbackDeviceIoControl: result=%p devctrl_callback=%p", qi_result, devctrl_callback);
    } else {
        log_verbose("com_create_io_queue has NO callback_iface at all!");
    }

    WDF_IO_QUEUE_CONFIG config = {0};
    config.Size = sizeof(config);
    config.DispatchType = dispatch_type;
    config.PowerManaged = power_managed;
    config.AllowZeroLengthRequests = allow_zero_len;
    config.DefaultQueue = default_queue;
    if(devctrl_callback) config.EvtIoDeviceControl = com_queue_evt_io_device_control;

    WDF_DRIVER_GLOBALS globals = { .Driver = wdf_device_get_driver(device) };
    WDFOBJECT raw_queue;
    NTSTATUS status = WdfIoQueueCreate(&globals, (WDFOBJECT) device, &config, NULL, &raw_queue);
    log_verbose("com_create_io_queue WdfIoQueueCreate status=0x%x raw_queue=%p", status, (void*) raw_queue);
    if(status != 0) return (void*) (intptr_t) status;

    struct com_obj *queue_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(queue_facade, raw_queue);
    wdf_queue_set_com_bridge((struct winwdf_queue*) raw_queue, devctrl_callback, queue_facade);

    *out = queue_facade;
    return (void*) (intptr_t) status;
}

//IWDFInterrupt - identified live: IWDFDevice3::CreateInterrupt (slot 40) failing with E_NOTIMPL left
//the driver's own cached interrupt-object field (device context +0xd0, NULL from construction) unset,
//and later code unconditionally called through it (vtable slot 17, TryToAcquireInterruptLock),
//crashing on the NULL deref. This driver doesn't have a real line-based IRQ (it's a plain USB
//device) - real WDF drivers commonly still create a WDF interrupt object purely to get its built-in
//synchronization semantics (AcquireLock/ReleaseLock/TryToAcquireInterruptLock etc. around a
//dedicated lock), so a working facade backed by a real mutex - not a real interrupt - is exactly
//what's needed here, not actual interrupt wiring.
struct com_interrupt_data {
    pthread_mutex_t lock;
    struct com_obj *device_obj;
};

__winfnc static void com_interrupt_acquire_lock(struct com_obj *self) {
    cant_fail_ret(pthread_mutex_lock(&((struct com_interrupt_data*) self->user_data)->lock));
}
__winfnc static void com_interrupt_release_lock(struct com_obj *self) {
    cant_fail_ret(pthread_mutex_unlock(&((struct com_interrupt_data*) self->user_data)->lock));
}
__winfnc static void com_interrupt_enable(struct com_obj *self) {}
__winfnc static void com_interrupt_disable(struct com_obj *self) {}
__winfnc static struct com_obj *com_interrupt_get_device(struct com_obj *self) {
    return ((struct com_interrupt_data*) self->user_data)->device_obj;
}
__winfnc static void com_interrupt_get_info(struct com_obj *self, void *info_out) {}
__winfnc static void com_interrupt_set_policy(struct com_obj *self, ULONG policy, ULONG priority, ULONG_PTR affinity) {}
__winfnc static void com_interrupt_set_extended_policy(struct com_obj *self, void *policy_and_group) {}
__winfnc static BOOLEAN com_interrupt_queue_workitem_for_isr(struct com_obj *self) { return FALSE; }
//"Try" must be non-blocking - a real ISR calls this specifically to avoid deadlocking against a
//DPC/worker thread that's already holding the same lock (real KMDF interrupt locks are
//non-recursive). This previously called the *blocking* pthread_mutex_lock, which is
//indistinguishable from AcquireLock/AcquireInterruptLock (same underlying function) - any caller
//that legitimately expected a fast non-blocking probe here (e.g. a worker thread checking whether
//the "interrupt" path is busy before doing real capture work) would hang forever instead, with no
//crash and no further log output, which matches exactly what was observed live: StartCapture's
//worker threads spawning, doing nothing else, and only ever going away via the devctrl watchdog's
//forced timeout.
__winfnc static BOOLEAN com_interrupt_try_acquire_interrupt_lock(struct com_obj *self) {
    BOOLEAN got = pthread_mutex_trylock(&((struct com_interrupt_data*) self->user_data)->lock) == 0;
    return got;
}

__winfnc static void *com_create_interrupt(struct com_obj *self, void *config, struct com_obj **out) {
    log_verbose("com_create_interrupt ENTER self=%p", self);
    struct com_interrupt_data *data = (struct com_interrupt_data*) malloc(sizeof(struct com_interrupt_data));
    cant_fail_ret(pthread_mutex_init(&data->lock, NULL));
    data->device_obj = self;

    struct com_obj *obj = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(obj, data);
    obj->vtable[6] = com_interrupt_acquire_lock;             //IWDFInterrupt::AcquireLock
    obj->vtable[7] = com_interrupt_release_lock;              //IWDFInterrupt::ReleaseLock
    obj->vtable[8] = com_interrupt_enable;                    //IWDFInterrupt::Enable
    obj->vtable[9] = com_interrupt_disable;                   //IWDFInterrupt::Disable
    obj->vtable[10] = com_interrupt_get_device;               //IWDFInterrupt::GetDevice
    obj->vtable[11] = com_interrupt_get_info;                 //IWDFInterrupt::GetInfo
    obj->vtable[12] = com_interrupt_set_policy;                //IWDFInterrupt::SetPolicy
    obj->vtable[13] = com_interrupt_set_extended_policy;      //IWDFInterrupt::SetExtendedPolicy
    obj->vtable[14] = com_interrupt_acquire_lock;             //IWDFInterrupt::AcquireInterruptLock
    obj->vtable[15] = com_interrupt_release_lock;             //IWDFInterrupt::ReleaseInterruptLock
    obj->vtable[16] = com_interrupt_queue_workitem_for_isr;   //IWDFInterrupt::QueueWorkItemForIsr
    obj->vtable[17] = com_interrupt_try_acquire_interrupt_lock; //IWDFInterrupt::TryToAcquireInterruptLock

    *out = obj;
    return 0; //S_OK
}

//IWDFUsbTargetFactory/Device/Interface/Pipe - the documented UMDF 1.x USB access chain (see
//g_iid_iwdf_usb_target_factory above). Delegates to the already-working, libusb-backed flat-table
//implementation in winapi/wdf/usb.c wherever possible (WdfUsbTargetDeviceCreate,
//WdfUsbTargetDeviceGetInterface, WdfUsbInterfaceGetConfiguredPipe all take/return the exact same
//WDFOBJECT-typed handles regardless of which activation model reaches them, so no translation
//needed there); FormatRequestForRead/Write go through wdf_usb_pipe_format_request_for_{read,write}_raw
//instead of the flat WdfUsbTargetPipeFormatRequestForRead/Write, since those expect a real
//WDF_memory-shaped WDFOBJECT for pMemory, not one of this project's own IWDFMemory facades
//(com_memory_data) - see winapi/wdf/usb.c's transfer_req_ctx comment.
//
//user_data on each of these facades is the raw flat-table object pointer directly (struct
//wdf_usb_device*/wdf_usb_interface*/wdf_usb_pipe*) - no extra wrapper struct needed except for the
//pipe facade, which also caches its own WDF_USB_PIPE_INFORMATION (filled in by
//WdfUsbInterfaceGetConfiguredPipe at creation time) since GetInformation/IsInEndPoint/IsOutEndPoint/
//GetType all just report fields from it.
struct com_usb_pipe_data {
    struct wdf_usb_pipe *usb_pipe;
    WDF_USB_PIPE_INFORMATION info;
};

__winfnc static HRESULT com_usb_pipe_format_request_for_read(struct com_obj *self, struct com_obj *req_facade, struct com_obj *file_obj, struct com_obj *mem_facade, WDFMEMORY_OFFSET *mem_off, LONGLONG *device_offset) {
    struct com_usb_pipe_data *pipe_data = (struct com_usb_pipe_data*) self->user_data;
    struct com_request_data *rd = (struct com_request_data*) req_facade->user_data;
    struct com_memory_data *mem = (struct com_memory_data*) mem_facade->user_data;

    uint8_t *buf = (uint8_t*) mem->buf;
    size_t buf_len = mem->size;
    if(mem_off) { buf += mem_off->BufferOffset; buf_len = mem_off->BufferLength; }

    log_verbose("com_usb_pipe_format_request_for_read self=%p req=%p buf=%p len=0x%zx", self, rd->req, (void*) buf, buf_len);
    NTSTATUS status = wdf_usb_pipe_format_request_for_read_raw(pipe_data->usb_pipe, rd->req, buf, buf_len);
    return (HRESULT) (intptr_t) status; //STATUS_SUCCESS (0) doubles as S_OK
}
__winfnc static HRESULT com_usb_pipe_format_request_for_write(struct com_obj *self, struct com_obj *req_facade, struct com_obj *file_obj, struct com_obj *mem_facade, WDFMEMORY_OFFSET *mem_off, LONGLONG *device_offset) {
    struct com_usb_pipe_data *pipe_data = (struct com_usb_pipe_data*) self->user_data;
    struct com_request_data *rd = (struct com_request_data*) req_facade->user_data;
    struct com_memory_data *mem = (struct com_memory_data*) mem_facade->user_data;

    uint8_t *buf = (uint8_t*) mem->buf;
    size_t buf_len = mem->size;
    if(mem_off) { buf += mem_off->BufferOffset; buf_len = mem_off->BufferLength; }

    log_verbose("com_usb_pipe_format_request_for_write self=%p req=%p buf=%p len=0x%zx", self, rd->req, (void*) buf, buf_len);
    NTSTATUS status = wdf_usb_pipe_format_request_for_write_raw(pipe_data->usb_pipe, rd->req, buf, buf_len);
    return (HRESULT) (intptr_t) status;
}
//Real Windows WINUSB_PIPE_INFORMATION layout (a plain data struct the driver reads directly, not a
//COM object) - matches winapi/winusb.c's own private copy, redeclared here to avoid a cross-file
//dependency for four fields.
typedef struct { UCHAR PipeType; UCHAR PipeId; USHORT MaximumPacketSize; UCHAR Interval; } COM_WINUSB_PIPE_INFORMATION;
__winfnc static void com_usb_pipe_get_information(struct com_obj *self, COM_WINUSB_PIPE_INFORMATION *info_out) {
    struct com_usb_pipe_data *pipe_data = (struct com_usb_pipe_data*) self->user_data;
    *info_out = (COM_WINUSB_PIPE_INFORMATION) {
        .PipeType = (UCHAR) pipe_data->info.PipeType, .PipeId = pipe_data->info.EndpointAddress,
        .MaximumPacketSize = (USHORT) pipe_data->info.MaximumPacketSize, .Interval = pipe_data->info.Interval,
    };
}
__winfnc static BOOL com_usb_pipe_is_in_endpoint(struct com_obj *self) {
    return (((struct com_usb_pipe_data*) self->user_data)->info.EndpointAddress & 0x80) != 0;
}
__winfnc static BOOL com_usb_pipe_is_out_endpoint(struct com_obj *self) {
    return (((struct com_usb_pipe_data*) self->user_data)->info.EndpointAddress & 0x80) == 0;
}
__winfnc static ULONG com_usb_pipe_get_type(struct com_obj *self) {
    return (ULONG) ((struct com_usb_pipe_data*) self->user_data)->info.PipeType;
}

__winfnc static HRESULT com_usb_interface_retrieve_pipe(struct com_obj *self, UCHAR pipe_idx, struct com_obj **out) {
    struct wdf_usb_interface *usb_if = (struct wdf_usb_interface*) self->user_data;

    WDF_USB_PIPE_INFORMATION info = {0};
    WDFOBJECT pipe_obj = WdfUsbInterfaceGetConfiguredPipe(NULL, (WDFOBJECT) usb_if, pipe_idx, &info);
    log_verbose("com_usb_interface_retrieve_pipe self=%p pipe_idx=%d pipe_obj=%p", self, (int) pipe_idx, (void*) pipe_obj);
    if(!pipe_obj) return (HRESULT) 0x80070057; /* E_INVALIDARG */

    struct com_usb_pipe_data *pipe_data = (struct com_usb_pipe_data*) malloc(sizeof(struct com_usb_pipe_data));
    pipe_data->usb_pipe = (struct wdf_usb_pipe*) pipe_obj;
    pipe_data->info = info;

    struct com_obj *pipe_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(pipe_facade, pipe_data);
    pipe_facade->vtable[10] = com_usb_pipe_format_request_for_read;  //IWDFUsbTargetPipe::FormatRequestForRead
    pipe_facade->vtable[11] = com_usb_pipe_format_request_for_write; //IWDFUsbTargetPipe::FormatRequestForWrite
    pipe_facade->vtable[16] = com_usb_pipe_get_information;          //IWDFUsbTargetPipe::GetInformation
    pipe_facade->vtable[17] = com_usb_pipe_is_in_endpoint;           //IWDFUsbTargetPipe::IsInEndPoint
    pipe_facade->vtable[18] = com_usb_pipe_is_out_endpoint;          //IWDFUsbTargetPipe::IsOutEndPoint
    pipe_facade->vtable[19] = com_usb_pipe_get_type;                 //IWDFUsbTargetPipe::GetType

    *out = pipe_facade;
    return 0; //S_OK
}

__winfnc static UCHAR com_usb_device_get_num_interfaces(struct com_obj *self) {
    return (UCHAR) wdf_usb_device_get_num_interfaces((struct wdf_usb_device*) self->user_data);
}
__winfnc static HRESULT com_usb_device_retrieve_interface(struct com_obj *self, UCHAR idx, struct com_obj **out) {
    struct wdf_usb_device *usb_dev = (struct wdf_usb_device*) self->user_data;

    WDFOBJECT if_obj = WdfUsbTargetDeviceGetInterface(NULL, (WDFOBJECT) usb_dev, idx);
    log_verbose("com_usb_device_retrieve_interface self=%p idx=%d if_obj=%p", self, (int) idx, (void*) if_obj);
    if(!if_obj) return (HRESULT) 0x80070057; /* E_INVALIDARG */

    struct com_obj *if_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(if_facade, if_obj);
    if_facade->vtable[14] = com_usb_interface_retrieve_pipe; //IWDFUsbInterface::RetrieveUsbPipeObject

    *out = if_facade;
    return 0; //S_OK
}

__winfnc static HRESULT com_usb_factory_create_device(struct com_obj *self, struct com_obj **out) {
    struct winwdf_device *device = (struct winwdf_device*) self->user_data;

    WDF_DRIVER_GLOBALS globals = { .Driver = wdf_device_get_driver(device) };
    WDFOBJECT usb_dev_obj;
    NTSTATUS status = WdfUsbTargetDeviceCreate(&globals, (WDFOBJECT) device, NULL, &usb_dev_obj);
    log_verbose("com_usb_factory_create_device self=%p status=0x%x usb_dev_obj=%p", self, status, (void*) usb_dev_obj);
    if(status != 0) return (HRESULT) (intptr_t) status;

    struct com_obj *dev_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(dev_facade, usb_dev_obj);
    dev_facade->vtable[14] = com_usb_device_get_num_interfaces;  //IWDFUsbTargetDevice::GetNumInterfaces
    dev_facade->vtable[15] = com_usb_device_retrieve_interface;  //IWDFUsbTargetDevice::RetrieveUsbInterface

    *out = dev_facade;
    return 0; //S_OK
}

static struct com_obj *get_usb_target_factory_facade(struct winwdf_device *device) {
    static struct com_obj *factory_facade = NULL;
    if(!factory_facade) {
        factory_facade = (struct com_obj*) malloc(sizeof(struct com_obj));
        make_com_obj(factory_facade, device);
        factory_facade->vtable[3] = com_usb_factory_create_device; //IWDFUsbTargetFactory::CreateUsbTargetDevice
        log_verbose("created IWDFUsbTargetFactory facade %p for device %p", factory_facade, device);
    }
    return factory_facade;
}

//IWDFDevice::ConfigureRequestDispatching - identified live via the same vtable-math cross-reference
//as CreateIoQueue above (RetrieveDeviceName, PostEvent, then ConfigureRequestDispatching lands on
//slot 19), matching the observed arguments: (pQueue, RequestType, Forward).
//
//RequestType here is UMDF's own WDF_REQUEST_TYPE (wudfddi_types.h: WdfRequestCreate=1, ...,
//WdfRequestDeviceIoControl=5, ...) - a *completely different* value space from the kernel-style
//WDF_REQUEST_TYPE the flat-table WdfDeviceConfigureRequestDispatching below actually expects
//(winwdf.h: WdfRequestTypeCreate=0, ..., WdfRequestTypeDeviceControl=14, ...), despite sharing a
//type name. Passing the raw value straight through silently mapped every real request type (e.g.
//the value 5 the driver was observed registering, WdfRequestDeviceIoControl) onto whatever
//unrelated kernel-style request type happens to share that same number, so devctrl dispatch was
//never actually getting configured - hence Reset()'s IOCTL still failing with no queue registered
//even after CreateIoQueue itself started working. Mapped explicitly instead; request types we have
//no dispatch tracking for are silently accepted as a no-op rather than rejected, since our
//simplified WDF file model has no equivalent concept for most of them.
__winfnc static void *com_configure_request_dispatching(struct com_obj *self, struct com_obj *queue_facade, ULONG req_type, BOOL forward) {
    log_verbose("com_configure_request_dispatching ENTER self=%p req_type=%u", self, req_type);
    struct winwdf_device *device = (struct winwdf_device*) self->user_data;
    if(!queue_facade) return 0;
    WDFOBJECT raw_queue = (WDFOBJECT) queue_facade->user_data;

    WDF_REQUEST_TYPE mapped_type;
    switch(req_type) {
        case 1: /* WdfRequestCreate */ mapped_type = WdfRequestTypeCreate; break;
        case 3: /* WdfRequestRead */ mapped_type = WdfRequestTypeRead; break;
        case 4: /* WdfRequestWrite */ mapped_type = WdfRequestTypeWrite; break;
        case 5: /* WdfRequestDeviceIoControl */ mapped_type = WdfRequestTypeDeviceControl; break;
        case 9: /* WdfRequestInternalIoctl */ mapped_type = WdfRequestTypeDeviceControlInternal; break;
        default: return 0; //Not a request type we track dispatch for - treat as handled/no-op.
    }

    WDF_DRIVER_GLOBALS globals = { .Driver = wdf_device_get_driver(device) };
    return (void*) (intptr_t) WdfDeviceConfigureRequestDispatching(&globals, (WDFOBJECT) device, raw_queue, mapped_type);
}

//g_expected_dev_init_obj guards against a dev_init_obj we don't recognize (defensive only - every
//call observed live so far has matched the IWDFDeviceInitialize we handed the driver via OnDeviceAdd).
static struct com_obj *g_expected_dev_init_obj;

//Set by com_create_device, consumed by tudor_com_add_device once on_device_add fully returns - see
//the comment in com_create_device for why OnPrepareHardware can't be invoked synchronously from
//inside CreateDevice itself.
static void *g_pending_pnp_callback_iface;
static struct com_obj *g_pending_device_obj;

typedef void *__winfnc (*com_pnp_prepare_hw_fnc)(void *self, void *wdf_device);
typedef void *__winfnc (*com_pnp_d0_entry_fnc)(void *self, void *wdf_device, ULONG previous_state);
typedef void *__winfnc (*com_pnp_d0_exit_fnc)(void *self, void *wdf_device, ULONG new_state);
typedef void *__winfnc (*com_pnp_release_hw_fnc)(void *self, void *wdf_device);

//Kept after tudor_com_add_device for tudor_com_remove_device's teardown callbacks
static void *g_pnp_callback_iface;
static struct com_obj *g_device_obj;

__winfnc static void *com_create_device(struct com_obj *self, struct com_obj *dev_init_obj, void *pnp_callback_iface, WDFOBJECT *out) {
    if(pnp_callback_iface) com_raw_add_ref(pnp_callback_iface);

    if(dev_init_obj != g_expected_dev_init_obj) {
        log_warn("IWDFDriver::CreateDevice called with an unrecognized DeviceInit (%p) - ignoring nested/unexpected device creation.", dev_init_obj);
        return (void*) (intptr_t) 0xC0000001; /* STATUS_UNSUCCESSFUL */
    }

    struct winwdf_driver *driver = (struct winwdf_driver*) self->user_data;
    struct wdf_device_init *dev_init = (struct wdf_device_init*) dev_init_obj->user_data;

    //pnp_callback_iface must NOT be forwarded to our own WdfDeviceCreate as a WDF_OBJECT_ATTRIBUTES*
    //(which is what this argument slot is in the flat-table WdfDeviceCreate signature this delegates
    //to) - it's genuinely a different type here (see g_iid_ipnp_callback_hardware above). Passing it
    //through made wdf_create_obj do `obj->attrs = *attrs`, copying garbage COM-object bytes into
    //obj->attrs.EvtCleanupCallback/EvtDestroyCallback, which wdf_cleanup_obj later called as real
    //function pointers - crashing on teardown (observed live: tudor_close() jumping to a garbage
    //address like 0x31 read out of the COM object).
    WDF_DRIVER_GLOBALS globals = { .Driver = driver };
    WDFOBJECT raw_device;
    NTSTATUS status = WdfDeviceCreate(&globals, &dev_init, NULL, &raw_device);
    if(status != 0) return (void*) (intptr_t) status;

    struct com_obj *device_obj = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(device_obj, raw_device);
    device_obj->vtable[8] = com_retrieve_device_property_store; //IWDFDevice::RetrieveDevicePropertyStore
    device_obj->vtable[10] = com_retrieve_device_instance_id;  //IWDFDevice::RetrieveDeviceInstanceId
    device_obj->vtable[11] = com_get_default_io_target;       //IWDFDevice::GetDefaultIoTarget
    device_obj->vtable[14] = com_create_io_queue;             //IWDFDevice::CreateIoQueue
    device_obj->vtable[17] = com_retrieve_device_name;         //IWDFDevice::RetrieveDeviceName
    device_obj->vtable[19] = com_configure_request_dispatching; //IWDFDevice::ConfigureRequestDispatching
    device_obj->vtable[35] = com_map_io_space;                 //IWDFDevice3::MapIoSpace
    device_obj->vtable[40] = com_create_interrupt;             //IWDFDevice3::CreateInterrupt
    device_obj->iface_tag = COM_IFACE_IWDFDEVICE;
    *out = (WDFOBJECT) device_obj;

    //IPnpCallbackHardware::OnPrepareHardware is NOT invoked here, even though pnp_callback_iface
    //(g_iid_ipnp_callback_hardware) is available at this point - identified live: the driver's own
    //OnDeviceAdd implementation (on_device_add_fnc, still executing at this point, one frame up)
    //does further initialization of its own on pnp_callback_iface's underlying object AFTER
    //CreateDevice returns but BEFORE OnDeviceAdd itself returns (observed live: calling
    //OnPrepareHardware synchronously from inside CreateDevice crashed inside the driver's own real
    //code, reading a device-context field - offset +0xd0 - that's zeroed at construction and only
    //populated by that later, still-pending initialization). So the callback interface and device
    //are just stashed here; tudor_com_add_device below invokes OnPrepareHardware itself, only after
    //on_device_add has fully returned. The AddRef above already covers this reference's lifetime.
    g_pending_pnp_callback_iface = pnp_callback_iface;
    g_pending_device_obj = device_obj;

    return (void*) (intptr_t) status;
}

typedef void *__winfnc (*dllgetclassobject_fnc)(const GUID *clsid, const GUID *iid, void **out);
typedef void *__winfnc (*create_instance_fnc)(void *self, void *outer, const GUID *iid, void **out);
typedef void *__winfnc (*on_initialize_fnc)(void *self, void *wdf_driver);
typedef void *__winfnc (*on_device_add_fnc)(void *self, void *wdf_driver, void *wdf_device_init);

//Persists between tudor_init_com_driver (OnInitialize) and tudor_com_add_device (OnDeviceAdd);
//real IDriverEntry/IWDFDriver implementations are expected to remain valid across both calls,
//same as on real Windows.
static void *g_driver_entry;
static void **g_entry_vtable;
static struct com_obj *g_wdf_driver_obj;

bool tudor_init_com_driver(struct dll_image *driver_image, HANDLE reg_key, struct winwdf_driver **out_driver) {
    dllgetclassobject_fnc get_class_obj = (dllgetclassobject_fnc) find_dll_export(driver_image, "DllGetClassObject");
    if(!get_class_obj) {
        log_error("Driver DLL doesn't export DllGetClassObject either!");
        return false;
    }

    //From synaWudfBioUsbPQIDongleProd.inf's [synaWudfBioUsb52_Install] section.
    const GUID driver_clsid = DEFINE_GUID(96710705, b080, 4b29, a3ec, b16935ae663a);
    const GUID iid_unknown = DEFINE_GUID(00000000, 0000, 0000, c000, 000000000046ull);

    void *class_factory;
    if(get_class_obj(&driver_clsid, &iid_unknown, &class_factory) != 0 || !class_factory) {
        log_error("DllGetClassObject failed to produce a class factory!");
        return false;
    }

    //IClassFactory::CreateInstance is a stable, universal COM ABI slot (index 3, right after
    //IUnknown's 3 methods) - unlike the driver-specific IDriverEntry/IWDFDriver interfaces, this
    //doesn't need to be guessed at.
    void **factory_vtable = *(void***) class_factory;
    create_instance_fnc create_instance = (create_instance_fnc) factory_vtable[3];

    if(create_instance(class_factory, NULL, &iid_unknown, &g_driver_entry) != 0 || !g_driver_entry) {
        log_error("IClassFactory::CreateInstance failed to produce a driver entry object!");
        return false;
    }
    g_entry_vtable = *(void***) g_driver_entry;

    struct winwdf_driver *driver = winwdf_create_driver_for_com(reg_key);
    if(!driver) { log_error("Couldn't create WDF driver object!"); return false; }

    g_wdf_driver_obj = (struct com_obj*) malloc(sizeof(struct com_obj));
    make_com_obj(g_wdf_driver_obj, driver);
    //Identified live (see com_create_device) - overrides the generic stub for this one slot.
    g_wdf_driver_obj->vtable[8] = com_create_device;
    g_wdf_driver_obj->iface_tag = COM_IFACE_IWDFDRIVER;

    on_initialize_fnc on_initialize = (on_initialize_fnc) g_entry_vtable[3];
    g_stub_iface_name = "IWDFDriver";
    NTSTATUS status = (NTSTATUS)(intptr_t) on_initialize(g_driver_entry, g_wdf_driver_obj);
    if(status != 0) {
        log_error("IDriverEntry::OnInitialize failed: 0x%x!", status);
        return false;
    }

    *out_driver = driver;
    return true;
}

NTSTATUS tudor_com_add_device(HANDLE reg_key, libusb_device_handle *usb_dev, struct winwdf_device **out_dev) {
    struct winwdf_driver *driver = (struct winwdf_driver*) g_wdf_driver_obj->user_data;

    *out_dev = NULL;
    struct wdf_device_init *dev_init = wdf_create_device_init(driver, reg_key, usb_dev, out_dev);
    if(!dev_init) return winerr_from_errno();

    //IWDFDeviceInitialize is only valid for the duration of this call on real Windows too, so a
    //stack-local COM object here is correct, not just convenient.
    struct com_obj wdf_device_init_obj;
    make_com_obj(&wdf_device_init_obj, dev_init);

    on_device_add_fnc on_device_add = (on_device_add_fnc) g_entry_vtable[4];
    g_stub_iface_name = "IWDFDeviceInitialize";
    g_expected_dev_init_obj = &wdf_device_init_obj;
    g_pending_pnp_callback_iface = NULL;
    g_pending_device_obj = NULL;
    NTSTATUS status = (NTSTATUS)(intptr_t) on_device_add(g_driver_entry, g_wdf_driver_obj, &wdf_device_init_obj);
    g_expected_dev_init_obj = NULL;

    //Real UMDF hosts QueryInterface pCallbackInterface for IPnpCallbackHardware and invoke
    //OnPrepareHardware as part of the PnP AddDevice sequence - but only once OnDeviceAdd (i.e. this
    //very call) has fully returned, since the driver's own OnDeviceAdd code does further
    //initialization of pCallbackInterface's underlying object after CreateDevice returns but before
    //OnDeviceAdd itself does (see com_create_device's comment for how this was identified live).
    if(status == 0 && g_pending_pnp_callback_iface) {
        g_pnp_callback_iface = g_pending_pnp_callback_iface;
        g_device_obj = g_pending_device_obj;
        void **cb_vtable = *(void***) g_pending_pnp_callback_iface;
        void *pnp_hw_iface = NULL;
        if(((com_raw_qi_fnc) cb_vtable[0])(g_pending_pnp_callback_iface, &g_iid_ipnp_callback_hardware, &pnp_hw_iface) == 0 && pnp_hw_iface) {
            void **hw_vtable = *(void***) pnp_hw_iface;
            HRESULT hw_status = (HRESULT)(intptr_t) ((com_pnp_prepare_hw_fnc) hw_vtable[3])(pnp_hw_iface, g_pending_device_obj);
            if(hw_status != 0) log_warn("IPnpCallbackHardware::OnPrepareHardware failed: 0x%x!", hw_status);
        } else {
            log_warn("Driver's CreateDevice callback interface doesn't support IPnpCallbackHardware!");
        }

        //IPnpCallback::OnD0Entry - the power-up transition real WDF always runs after hardware prep,
        //before the device is considered usable. previousState=WdfPowerDeviceD3 matches a freshly
        //added device's real starting state (powered off) on real Windows.
        void *pnp_iface = NULL;
        if(((com_raw_qi_fnc) cb_vtable[0])(g_pending_pnp_callback_iface, &g_iid_ipnp_callback, &pnp_iface) == 0 && pnp_iface) {
            void **pnp_vtable = *(void***) pnp_iface;
            HRESULT d0_status = (HRESULT)(intptr_t) ((com_pnp_d0_entry_fnc) pnp_vtable[3])(pnp_iface, g_pending_device_obj, 4 /* WdfPowerDeviceD3 */);
            if(d0_status != 0) log_warn("IPnpCallback::OnD0Entry failed: 0x%x!", d0_status);
        } else {
            log_warn("Driver's CreateDevice callback interface doesn't support IPnpCallback!");
        }
    }

    free(wdf_device_init_obj.vtable);
    return status;
}

//Mirror of tudor_com_add_device's power-up sequence: real UMDF runs IPnpCallback::OnD0Exit and then
//IPnpCallbackHardware::OnReleaseHardware when a device is removed. The driver uses these to stop its
//own worker threads and quiesce the sensor; skipping them let a still-running capture worker touch
//the DLL's heap after the driver had been unloaded (crash on close after a cancelled capture).
void tudor_com_remove_device(void) {
    if(!g_pnp_callback_iface) return;
    void **cb_vtable = *(void***) g_pnp_callback_iface;

    void *pnp_iface = NULL;
    if(((com_raw_qi_fnc) cb_vtable[0])(g_pnp_callback_iface, &g_iid_ipnp_callback, &pnp_iface) == 0 && pnp_iface) {
        log_debug("Calling IPnpCallback::OnD0Exit...");
        HRESULT st = (HRESULT)(intptr_t) ((com_pnp_d0_exit_fnc) (*(void***) pnp_iface)[4])(pnp_iface, g_device_obj, 5 /* WdfPowerDeviceD3Final */);
        if(st != 0) log_warn("IPnpCallback::OnD0Exit failed: 0x%x!", st);
    }

    void *pnp_hw_iface = NULL;
    if(((com_raw_qi_fnc) cb_vtable[0])(g_pnp_callback_iface, &g_iid_ipnp_callback_hardware, &pnp_hw_iface) == 0 && pnp_hw_iface) {
        log_debug("Calling IPnpCallbackHardware::OnReleaseHardware...");
        HRESULT st = (HRESULT)(intptr_t) ((com_pnp_release_hw_fnc) (*(void***) pnp_hw_iface)[4])(pnp_hw_iface, g_device_obj);
        if(st != 0) log_warn("IPnpCallbackHardware::OnReleaseHardware failed: 0x%x!", st);
    }

    g_pnp_callback_iface = NULL;
    g_device_obj = NULL;
}
