#include <sys/mman.h>
#include "internal.h"

extern uint8_t _binary_libtudor_synaBscAdapter52_dll_start, _binary_libtudor_synaBscAdapter52_dll_end;
extern uint8_t _binary_libtudor_synaWudfBioUsb52_dll_start, _binary_libtudor_synaWudfBioUsb52_dll_end;

//Patches the driver's initial WINBIO_SENSOR_STATUS from BUSY to READY - nothing reachable through
//the WDF/COM surface this project bridges ever clears it otherwise. See FINDINGS.md for the full
//investigation.
//
//Entries 2-3: NOP out two write instructions (RVA 0x4a11c/`mov [rax+0x8],rcx` and 0x4a12a/
//`mov [rax+0x10],rcx`) that crash on ctx->field_0x100's real object - `rax` in both is that object's
//own first 8 bytes (a type tag + flags pair the object's real constructor deliberately writes as two
//32-bit fields, not a pointer), read and treated as a pointer by this consumer, an unresolved
//real-driver layout disagreement. See FINDINGS.md "a real layout mismatch" for the full trace - this
//is a targeted workaround, not a fix for the underlying disagreement.
struct driver_patch { uint32_t rva; uint8_t expected[4], patched[4]; uint8_t len; const char *descr; };
static const struct driver_patch g_driver_patches[] = {
    { 0x19778, {0x04}, {0x03}, 1, "device-context initial WINBIO_SENSOR_STATUS (BUSY -> READY)" },
    { 0x4a11c, {0x48, 0x89, 0x48, 0x08}, {0x90, 0x90, 0x90, 0x90}, 4, "NOP out crashing write through ctx->field_0x100's tag+flags read as a pointer (offset+0x8)" },
    { 0x4a12a, {0x48, 0x89, 0x48, 0x10}, {0x90, 0x90, 0x90, 0x90}, 4, "NOP out crashing write through ctx->field_0x100's tag+flags read as a pointer (offset+0x10)" },
};

static void apply_driver_patches(struct dll_image *image) {
    for(size_t i = 0; i < sizeof(g_driver_patches) / sizeof(*g_driver_patches); i++) {
        const struct driver_patch *patch = &g_driver_patches[i];
        //The NOPs only make sense against the legacy fake ctx->field_0x100 (TUDOR_LEGACY_CTX_PATCH) -
        //with the driver's own object they drop the capture worker's finger-event callback
        //registration (0x4a0a0). See FINDINGS.md.
        if(!getenv("TUDOR_LEGACY_CTX_PATCH") && patch->patched[0] == 0x90) continue;
        uint8_t *addr = (uint8_t*) image->base_addr + patch->rva;

        uint8_t *page = (uint8_t*) ((uintptr_t) addr & ~(uintptr_t) 0xFFF);
        //Patches are all small (<=4 bytes); mprotect two pages in the rare case one straddles a
        //boundary, harmless overkill otherwise since both are already part of the same image mapping.
        if(mprotect(page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("mprotect (patch rw)"); abort(); }

        if(memcmp(addr, patch->expected, patch->len) != 0) {
            log_error("Driver patch '%s' at RVA 0x%x: expected bytes didn't match - refusing to patch (driver build likely changed)!", patch->descr, patch->rva);
            abort();
        }
        memcpy(addr, patch->patched, patch->len);
        log_warn("Applied driver patch: %s", patch->descr);

        if(mprotect(page, 0x2000, PROT_READ | PROT_EXEC)) { perror("mprotect (patch restore)"); abort(); }
    }
}

//Fake object for ctx->field_0x1b8 (the StartCapture worker thread's context object, gated behind
//ctx->field_0x148/0x144 being nonzero - see FINDINGS.md). Every vtable slot is a logging stub
//returning 0, enough to satisfy the gate without crashing on an unimplemented slot.
#define FAKE_CTX_OBJ_SLOTS 48
struct fake_ctx_obj { void **vtable; };
static void *g_fake_ctx_obj_vtable[FAKE_CTX_OBJ_SLOTS];

static void *fake_ctx_obj_stub_impl(int slot, void *self, void *a, void *a2, void *a3, void *a4) {
    log_debug("DIAG: fake StartCapture-context object vtable slot #%d called! [self=%p args: %p %p %p %p]", slot, self, a, a2, a3, a4);
    return 0;
}
//Slot #2 is live-confirmed (not statically inferred) as a property-get call: a in the 0x938ae0XX
//family (matching the property-key constants found deep in the real driver's own status-translation
//chain - see FINDINGS.md), a2 an output buffer, a3 its size. Every other slot leaves the caller's
//buffer untouched (uninitialized garbage on their stack); this was true here too before this change.
//Zeroing it is a deliberate, minimal improvement over garbage - not a guess at real semantics, since
//those aren't known yet - see FINDINGS.md for why 0 is a defensible default (raw 0 maps to a "still
//pending", not error, status in the one status-translation table this project has fully decoded).
__winfnc static void *fake_ctx_obj_stub_2(void *self, void *a, void *a2, void *a3, void *a4) {
    log_debug("DIAG: fake StartCapture-context object vtable slot #2 called! [self=%p args: %p %p %p %p]", self, a, a2, a3, a4);
    size_t sz = (size_t) a3;
    if(a2 && sz > 0 && sz <= 0x1000) memset(a2, 0, sz);
    return 0;
}
#define FAKE_CTX_STUB(n) __winfnc static void *fake_ctx_obj_stub_##n(void *self, void *a, void *a2, void *a3, void *a4) { return fake_ctx_obj_stub_impl(n, self, a, a2, a3, a4); }
FAKE_CTX_STUB(0)  FAKE_CTX_STUB(1)                     FAKE_CTX_STUB(3)  FAKE_CTX_STUB(4)  FAKE_CTX_STUB(5)  FAKE_CTX_STUB(6)  FAKE_CTX_STUB(7)
FAKE_CTX_STUB(8)  FAKE_CTX_STUB(9)  FAKE_CTX_STUB(10) FAKE_CTX_STUB(11) FAKE_CTX_STUB(12) FAKE_CTX_STUB(13) FAKE_CTX_STUB(14) FAKE_CTX_STUB(15)
FAKE_CTX_STUB(16) FAKE_CTX_STUB(17) FAKE_CTX_STUB(18) FAKE_CTX_STUB(19) FAKE_CTX_STUB(20) FAKE_CTX_STUB(21) FAKE_CTX_STUB(22) FAKE_CTX_STUB(23)
FAKE_CTX_STUB(24) FAKE_CTX_STUB(25) FAKE_CTX_STUB(26) FAKE_CTX_STUB(27) FAKE_CTX_STUB(28) FAKE_CTX_STUB(29) FAKE_CTX_STUB(30) FAKE_CTX_STUB(31)
FAKE_CTX_STUB(32) FAKE_CTX_STUB(33) FAKE_CTX_STUB(34) FAKE_CTX_STUB(35) FAKE_CTX_STUB(36) FAKE_CTX_STUB(37) FAKE_CTX_STUB(38) FAKE_CTX_STUB(39)
FAKE_CTX_STUB(40) FAKE_CTX_STUB(41) FAKE_CTX_STUB(42) FAKE_CTX_STUB(43) FAKE_CTX_STUB(44) FAKE_CTX_STUB(45) FAKE_CTX_STUB(46) FAKE_CTX_STUB(47)
#undef FAKE_CTX_STUB

static void init_fake_ctx_obj_vtable() {
    void *stubs[FAKE_CTX_OBJ_SLOTS] = {
        fake_ctx_obj_stub_0,  fake_ctx_obj_stub_1,  fake_ctx_obj_stub_2,  fake_ctx_obj_stub_3,  fake_ctx_obj_stub_4,  fake_ctx_obj_stub_5,  fake_ctx_obj_stub_6,  fake_ctx_obj_stub_7,
        fake_ctx_obj_stub_8,  fake_ctx_obj_stub_9,  fake_ctx_obj_stub_10, fake_ctx_obj_stub_11, fake_ctx_obj_stub_12, fake_ctx_obj_stub_13, fake_ctx_obj_stub_14, fake_ctx_obj_stub_15,
        fake_ctx_obj_stub_16, fake_ctx_obj_stub_17, fake_ctx_obj_stub_18, fake_ctx_obj_stub_19, fake_ctx_obj_stub_20, fake_ctx_obj_stub_21, fake_ctx_obj_stub_22, fake_ctx_obj_stub_23,
        fake_ctx_obj_stub_24, fake_ctx_obj_stub_25, fake_ctx_obj_stub_26, fake_ctx_obj_stub_27, fake_ctx_obj_stub_28, fake_ctx_obj_stub_29, fake_ctx_obj_stub_30, fake_ctx_obj_stub_31,
        fake_ctx_obj_stub_32, fake_ctx_obj_stub_33, fake_ctx_obj_stub_34, fake_ctx_obj_stub_35, fake_ctx_obj_stub_36, fake_ctx_obj_stub_37, fake_ctx_obj_stub_38, fake_ctx_obj_stub_39,
        fake_ctx_obj_stub_40, fake_ctx_obj_stub_41, fake_ctx_obj_stub_42, fake_ctx_obj_stub_43, fake_ctx_obj_stub_44, fake_ctx_obj_stub_45, fake_ctx_obj_stub_46, fake_ctx_obj_stub_47,
    };
    memcpy(g_fake_ctx_obj_vtable, stubs, sizeof(stubs));
}

//RVA of the StartCapture worker thread's start routine (winapi/thread.c's THREAD_START_ROUTINE
//argument to CreateThread), identified live via a breakpoint on CreateThread itself.
#define STARTCAPTURE_THREAD_RVA 0x1c21c

static struct fake_ctx_obj g_fake_obj;
static bool g_fake_obj_init = false;

static struct fake_ctx_obj *get_fake_ctx_obj() {
    if(!g_fake_obj_init) {
        init_fake_ctx_obj_vtable();
        g_fake_obj.vtable = g_fake_ctx_obj_vtable;
        g_fake_obj_init = true;
    }
    return &g_fake_obj;
}

//Fake object for ctx->field_0x100 - unlike field_0x1b8 above, its real consumer reads raw fields
//directly instead of going through a vtable, so a plain vtable-only fake object crashes. This is a
//"quine": every 8-byte slot points back at the block's own start, so any short chain of
//dereference-plus-small-offset safely resolves back into itself instead of wandering into unmapped
//memory. See FINDINGS.md.
//
//Enrollment's devctrl 0x44200c drives this through triple indirection: obj.slot0 ("vtable"-like)
//points at an object whose OWN field_0x8 gets InterlockedIncrement-style bumped as a refcount, while
//obj.slot1 separately points at an object whose slot0 dereferences back to itself, and THAT value is
//passed to EnterCriticalSection/LeaveCriticalSection. This AddRef+lock pattern recurses onto whatever
//object it lands on - confirmed live (hardware watchpoints, and repeated live deadlock backtraces
//with a real finger touch) on ctx->field_0x100's own object, the object at its slot0 ("inner"), the
//object at its slot1 ("cs"), AND (once inner/cs's own slot0 landing pad was reused as a single shared
//sink) on that shared sink too - reusing one landing pad across multiple parents lets one parent's
//AddRef corrupt a DIFFERENT parent's own protected slot1. Each "lockable" object needs an entirely
//PRIVATE (non-shared) slot0 landing pad, its own dedicated router so *(its slot1) still resolves back
//to itself, and its own real recursive mutex at slot3. See FINDINGS.md - this may need extending
//further (same live-debug technique) if a yet-deeper object turns out to also be locked directly.
#define FAKE_QUINE_OBJ_SLOTS 32
struct fake_quine_obj { void *slots[FAKE_QUINE_OBJ_SLOTS]; };

//A "lockable" quine object bundle: the object itself, a dedicated router that makes *(obj.slot1)
//resolve back to &obj without obj.slot0 needing to be self-referential, and a real mutex. obj.slot0
//is left for the caller to set to a PRIVATE (not shared with any other lockable_quine) landing pad.
struct lockable_quine { struct fake_quine_obj obj, selfptr_router; pthread_mutex_t mutex; };

//Confirmed live: the real driver calls free() on g_fake_quine_obj (as part of its own AddRef/Release
//lifecycle - see the AddRef discussion above) - a static/BSS pointer isn't a valid free() target and
//corrupts the heap allocator ("free(): invalid size"). So the whole family is heap-allocated (once,
//via calloc) instead of living in static storage, even though nothing currently re-allocates them if
//freed - see FINDINGS.md for the open question of whether a subsequent use-after-free also needs
//handling.
static struct fake_quine_obj *g_fake_quine_obj;                //ctx->field_0x100's own object
static struct fake_quine_obj *g_fake_quine_obj_terminal;        //plain quine - shared BOTTOM of both private chains below
static struct lockable_quine *g_fake_quine_obj_inner;            //reached via g_fake_quine_obj.slot0
static struct lockable_quine *g_fake_quine_obj_inner_sink;        //inner's OWN private slot0 landing pad
static struct lockable_quine *g_fake_quine_obj_cs;                 //reached via g_fake_quine_obj.slot1
static struct lockable_quine *g_fake_quine_obj_cs_sink;             //cs's OWN private slot0 landing pad

//Confirmed live: SOME consumers treat SOME of these objects as COM objects with an EMBEDDED vtable
//and call a slot directly (`call [obj+N]`) - confirmed at offset 0x10 (slot2) on g_fake_quine_obj_terminal
//specifically, and offset 0x20 (slot4) on g_fake_quine_obj_cs_sink specifically, via separate live
//traces, unlike the pointer-chasing/AddRef consumers this whole quine family otherwise handles. This
//is NOT a property of the quine "shape" in general - a DIFFERENT consumer double-dereferences
//g_fake_quine_obj_inner_sink's slot2 expecting a plain DATA pointer there (confirmed live: applying
//this fix to inner_sink too made that consumer read into a stub function's own machine code bytes and
//pass them as a CRITICAL_SECTION pointer, crashing pthread_mutex_lock). So this is applied to ONLY
//the two concretely-confirmed (object, slot) pairs, not blanket-applied to every lockable_quine. See
//FINDINGS.md - if a NEW object/offset is confirmed live the same way (breakpoint at the crash target's
//address, disassemble the caller for the `call [obj+N]` immediate), add it here just as narrowly.
static void make_slot2_callable(struct fake_quine_obj *obj) { obj->slots[2] = fake_ctx_obj_stub_2; }
static void make_slot4_callable(struct fake_quine_obj *obj) { obj->slots[4] = fake_ctx_obj_stub_4; }

static void init_recursive_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    cant_fail_ret(pthread_mutexattr_init(&attr));
    cant_fail_ret(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    cant_fail_ret(pthread_mutex_init(mutex, &attr));
    cant_fail_ret(pthread_mutexattr_destroy(&attr));
}

static void init_lockable_quine(struct lockable_quine *lq, struct fake_quine_obj *slot0_target) {
    for(int i = 0; i < FAKE_QUINE_OBJ_SLOTS; i++) lq->obj.slots[i] = &lq->obj;
    for(int i = 0; i < FAKE_QUINE_OBJ_SLOTS; i++) lq->selfptr_router.slots[i] = &lq->selfptr_router;

    init_recursive_mutex(&lq->mutex);
    lq->obj.slots[3] = &lq->mutex;

    lq->obj.slots[0] = slot0_target;
    lq->obj.slots[1] = &lq->selfptr_router;
    lq->selfptr_router.slots[0] = &lq->obj;
}

//Confirmed live: terminal is ALSO called directly as an EnterCriticalSection target (sect==&terminal,
//not reached through any slot1 resolution chain we control) - reached via g_fake_quine_obj_inner_sink
//and g_fake_quine_obj_cs_sink's shared slot0 redirect. Needs a real mutex like any lockable_quine, but
//doesn't need the full slot0/slot1 treatment since nothing observed so far derives a "resolve back to
//terminal" pointer from its own slot1. See FINDINGS.md.
static pthread_mutex_t *g_fake_quine_obj_terminal_mutex;

//Confirmed live: after switching the family to calloc (above), the real driver's Release() genuinely
//frees g_fake_quine_obj once its refcount hits zero (matching a normal COM object lifecycle) - caching
//a single instance across the whole process then double-frees it on the NEXT StartCapture cycle. So
//this allocates a brand new family on every call instead of caching one - it's only ever called once
//per cycle anyway (gated by ctx->field_0x100 being NULL in patch_device_context). This does mean any
//cycle where the driver does NOT release its own reference leaks the previous family - acceptable for
//now over a guaranteed crash. See FINDINGS.md.
static struct fake_quine_obj *get_fake_quine_obj() {
    g_fake_quine_obj = (struct fake_quine_obj*) calloc(1, sizeof(struct fake_quine_obj));
    g_fake_quine_obj_terminal = (struct fake_quine_obj*) calloc(1, sizeof(struct fake_quine_obj));
    g_fake_quine_obj_inner = (struct lockable_quine*) calloc(1, sizeof(struct lockable_quine));
    g_fake_quine_obj_inner_sink = (struct lockable_quine*) calloc(1, sizeof(struct lockable_quine));
    g_fake_quine_obj_cs = (struct lockable_quine*) calloc(1, sizeof(struct lockable_quine));
    g_fake_quine_obj_cs_sink = (struct lockable_quine*) calloc(1, sizeof(struct lockable_quine));
    g_fake_quine_obj_terminal_mutex = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));

    for(int i = 0; i < FAKE_QUINE_OBJ_SLOTS; i++) g_fake_quine_obj->slots[i] = g_fake_quine_obj;
    for(int i = 0; i < FAKE_QUINE_OBJ_SLOTS; i++) g_fake_quine_obj_terminal->slots[i] = g_fake_quine_obj_terminal;
    make_slot2_callable(g_fake_quine_obj_terminal);
    //slot4 is confirmed callable on every object tried so far (g_fake_quine_obj, inner_sink,
    //cs_sink) with no conflicting data-consumer found (unlike slot2) - applied broadly here.
    make_slot4_callable(g_fake_quine_obj);
    make_slot4_callable(g_fake_quine_obj_terminal);
    init_recursive_mutex(g_fake_quine_obj_terminal_mutex);
    g_fake_quine_obj_terminal->slots[3] = g_fake_quine_obj_terminal_mutex;

    init_lockable_quine(g_fake_quine_obj_inner_sink, g_fake_quine_obj_terminal);
    make_slot4_callable(&g_fake_quine_obj_inner_sink->obj);
    make_slot4_callable(&g_fake_quine_obj_inner_sink->selfptr_router);
    init_lockable_quine(g_fake_quine_obj_inner, &g_fake_quine_obj_inner_sink->obj);
    make_slot4_callable(&g_fake_quine_obj_inner->obj);
    make_slot4_callable(&g_fake_quine_obj_inner->selfptr_router);
    init_lockable_quine(g_fake_quine_obj_cs_sink, g_fake_quine_obj_terminal);
    make_slot4_callable(&g_fake_quine_obj_cs_sink->obj);
    make_slot4_callable(&g_fake_quine_obj_cs_sink->selfptr_router);
    init_lockable_quine(g_fake_quine_obj_cs, &g_fake_quine_obj_cs_sink->obj);
    make_slot4_callable(&g_fake_quine_obj_cs->obj);
    make_slot4_callable(&g_fake_quine_obj_cs->selfptr_router);

    g_fake_quine_obj->slots[0] = &g_fake_quine_obj_inner->obj;
    g_fake_quine_obj->slots[1] = &g_fake_quine_obj_cs->selfptr_router;

    return g_fake_quine_obj;
}

//Calls the real (unmodified) driver routine that constructs the object for ctx+0x1b8, given
//ctx+0x260 as its argument, instead of using a fake stub - see FINDINGS.md. __winfnc (ms_abi) is
//required: real Windows-ABI code takes its first param in RCX, not SysV's RDI.
#define CTX260_CTOR_RVA 0x16354
typedef void *__winfnc ctx260_ctor_fn(void *arg);

static void *try_construct_real_ctx_obj(uint8_t *ctx) {
    if(!tudor_driver_dll) return NULL;
    ctx260_ctor_fn *ctor = (ctx260_ctor_fn*) ((uint8_t*) tudor_driver_dll->image.base_addr + CTX260_CTOR_RVA);
    return ctor((void*) (ctx + 0x260));
}

//Real vtable-populating constructor for whatever object ctx->field_0x100 is meant to point to - found
//by searching the whole DLL for `mov [reg+0x100], reg2` writes (not stack-relative), which turned up
//exactly one QWORD write matching a real function-pointer initializer spanning offsets 0x70-0x110 and
//0x240 of its argument. Unlike CTX260_CTOR_RVA above, this doesn't allocate anything itself - it just
//populates an already-allocated buffer's fields, so this project allocates+zeroes one and passes it
//in. Returns 0 on success (nonzero, e.g. 0x6f, if its argument is NULL). See FINDINGS.md.
#define FIELD0X100_CTOR_RVA 0xf6e00
#define FIELD0X100_OBJ_SIZE 0x300
typedef uint32_t __winfnc field0x100_ctor_fn(void *arg);

//The REAL (only) caller of FIELD0X100_CTOR_RVA anywhere in the DLL - not this project's own code,
//found by searching for direct callers of RVA 0xf6e00. This is a generic type-tagged constructor
//dispatcher: (type_tag, target_buffer) -> status. Type tag 0xca matches the exact type tag/flags
//pair FIELD0X100_CTOR_RVA itself writes to its object's offset 0 - confirming this dispatcher's
//0xca case IS the real, intended way to construct this object, not a coincidence. Beyond calling
//FIELD0X100_CTOR_RVA, this dispatcher ALSO allocates a real 0xd0-byte sub-object, runs a real
//constructor on it (RVA 0xfb9c0), and stores it at the target's offset +0x248 - real construction
//work this project's own direct call to FIELD0X100_CTOR_RVA was skipping entirely. Does NOT appear
//to touch offset +0x8 (the field this project already quine-patches) directly - so that patch is
//kept as a fallback, but calling the real dispatcher instead of the bare constructor gives whatever
//consumes +0x248 real data instead of nothing. See FINDINGS.md.
#define FIELD0X100_DISPATCH_RVA 0xa0fc0
#define FIELD0X100_TYPE_TAG 0xca
typedef uint32_t __winfnc field0x100_dispatch_fn(uint32_t type_tag, void *self);

//FIELD0X100_DISPATCH_RVA above is itself only an inner piece of a THIRD, even-outer real factory
//function (found by searching for direct callers of RVA 0xa0fc0 - exactly two hits anywhere in the
//DLL, both passing a type-tag field read out of some caller-owned struct, not a hardcoded constant).
//This outer factory: validates the tag against a much broader whitelist (0x65/0x66/0x67/0xc9/0xca/
//a 0x190-0x1be range/0x1f5 - 0xca, this object's tag, is one of many cases, not a coincidence),
//allocates its OWN real 0x260-byte buffer (this project's own direct call to FIELD0X100_DISPATCH_RVA
//was passing an object THIS project allocated instead - skipped work, same bug class as before),
//calls FIELD0X100_DISPATCH_RVA on it, and - the real, previously-unknown missing step - if that
//succeeds, calls the freshly-constructed object's OWN embedded-vtable slots +0x70 and +0x80 before
//ever handing it back. +0x70 takes a second, caller-supplied argument (a zeroed 8-byte scratch
//buffer at both real call sites - not a preexisting real object, so a fresh zeroed local works here
//too); +0x80 just gets a fixed mode flag (1). This is real, live-confirmed initialization work
//(WPP-traced logging preambles, real branches, not stubs) this project's direct FIELD0X100_DISPATCH_
//RVA call was skipping entirely, on top of the allocation itself.
#define FIELD0X100_GENERIC_FACTORY_RVA 0x9ede0
typedef uint32_t __winfnc field0x100_generic_factory_fn(uint32_t type_tag, void *extra_param_out, void **out_obj);

static void *try_construct_real_field0x100_obj(void *ctx_1b8_obj) {
    if(!tudor_driver_dll) return NULL;

    field0x100_generic_factory_fn *factory = (field0x100_generic_factory_fn*) ((uint8_t*) tudor_driver_dll->image.base_addr + FIELD0X100_GENERIC_FACTORY_RVA);
    uint64_t extra_param_scratch = 0; //Zeroed scratch buffer - matches both real call sites' own setup.
    void *obj = NULL;
    uint32_t status = factory(FIELD0X100_TYPE_TAG, &extra_param_scratch, &obj);
    if(status != 0 || !obj) {
        log_debug("DIAG: real ctx+0x100 generic factory (RVA 0x%x) returned status 0x%x obj=%p, discarding", FIELD0X100_GENERIC_FACTORY_RVA, status, obj);
        return NULL;
    }
    log_debug("DIAG: real ctx+0x100 generic factory populated obj=%p field_0x8=%p field_0x240=%p field_0x248=%p extra_param_scratch=0x%lx", obj, *(void**) ((uint8_t*) obj + 0x8), *(void**) ((uint8_t*) obj + 0x240), *(void**) ((uint8_t*) obj + 0x248), (unsigned long) extra_param_scratch);

    //field_0x8 is left zero by the bare constructor/dispatcher alone, but the outer factory's own
    //+0x70 initializer call (see FIELD0X100_GENERIC_FACTORY_RVA above) DOES write something real
    //here now - live-confirmed non-null for the first time this project has ever seen. However, it's
    //string-shaped data (an ASCII version-string fragment, e.g. "QM ( 5." for one observed value),
    //not a pointer - and a *different* real consumer (RVA ~0x50251-0x50264, reached directly from
    //ctx->field_0x100 itself, distinct from field_0x8's OWN documented {func_ptr@+0x10, self@+0x18}
    //bound-callback consumer chain at 0x180061000) dereferences it as one, causing a real SIGSEGV -
    //live-confirmed. This looks like a genuine struct-layout disagreement (this offset may mean
    //different things to different consumers depending on some type/mode discriminant not yet
    //identified), not something this project's own code did wrong. Unconditionally overwriting with
    //the quine fallback (not just when null) keeps this project crash-free while that's investigated
    //further - a real forward-progress data point (a genuine value existing here at all is new), not
    //yet something safe to hand to every consumer as-is. See FINDINGS.md.
    //
    //Tried pointing field_0x8 at ctx->field_0x1b8's own real object instead of another quine (on the
    //hypothesis that field_0x8 references that same per-thread context object) - live-confirmed this
    //gets one real dereference level further before crashing (a genuinely different, deeper crash
    //site), but the crash consumer turned out to be walking an unrelated array of driver-internal
    //objects, not something specific to ctx->field_0x100 - see FINDINGS.md for the full result. Not a
    //fix; reverted to the unconditional quine to stay crash-free. ctx_1b8_obj is intentionally still
    //threaded through here (unused for now) so a future pass can pick this experiment back up without
    //re-plumbing it.
    (void) ctx_1b8_obj;
    *(void**) ((uint8_t*) obj + 0x8) = get_fake_quine_obj();

    //offset 0 is a type tag (0xca) + flags pair as the constructor itself writes it - but a
    //separate consumer (RVA 0x4a105+, see the NOP driver patches above) reads *obj as ONE 8-byte
    //pointer and dereferences through it. Overwriting it with a safe quine pointer satisfies that
    //consumer; not yet confirmed whether anything else relies on reading it back as the original
    //tag+flags integers. See FINDINGS.md.
    *(void**) obj = get_fake_quine_obj();
    return obj;
}

//RVA of the real "cancel/release active capture object" routine. Deliberately NOT called - it
//hangs the process when run against our fake object (see FINDINGS.md). tudor_reset_ctx100 below
//clears ctx->field_0x100 directly instead.
#define CTX100_CLEANUP_RVA 0x1c268

//Remembers the last device-context pointer patch_device_context ran on, so tudor_reset_ctx100 below
//can reach ctx->field_0x100 from device.c - see FINDINGS.md for why this indirection is needed.
static uint8_t *g_last_patched_ctx = NULL;

//Called from device.c before every StartCapture devctrl dispatch: clears the fake ctx->field_0x100
//a previous capture attempt left behind, so the next attempt doesn't immediately see "already in
//progress" (WINBIO_E_DATA_COLLECTION_IN_PROGRESS). See FINDINGS.md for why this clears the field
//directly instead of calling the real driver's own cleanup routine.
void tudor_reset_ctx100(void) {
    if(!g_last_patched_ctx || !getenv("TUDOR_LEGACY_CTX_PATCH")) return;
    if(!*(void**) (g_last_patched_ctx + 0x100)) return;
    log_debug("DIAG: clearing ctx+0x100 before new StartCapture dispatch, ctx=%p", g_last_patched_ctx);
    *(void**) (g_last_patched_ctx + 0x100) = NULL;
}

//Called from com_activate.c right as a devctrl request completes (Complete/CompleteWithInformation) -
//mirrors the real driver's own "complete then clear ctx->field_0x168" pattern (see FINDINGS.md's
//0x8009800B investigation: the real cleanup is a compare-and-clear virtual method, RVA 0x1f160, whose
//call site was never found). Clearing ctx->field_0x168 itself (not freeing the request facade it
//points at - that's still read by the driver after completion, see com_activate.c) at this natural
//completion point, rather than via a separate delayed callback, is the one variant of this fix not
//yet tried live as of the last update to this comment - see FINDINGS.md.
void tudor_reset_ctx168(void) {
    if(!g_last_patched_ctx) return;
    if(!*(void**) (g_last_patched_ctx + 0x168)) return;
    log_debug("DIAG: clearing ctx+0x168 on devctrl completion, ctx=%p", g_last_patched_ctx);
    *(void**) (g_last_patched_ctx + 0x168) = NULL;
}

//Shared by tudor_hook_thread_create below and the SIGSEGV handler further down - same patch,
//applied wherever we can get our hands on this device-context pointer.
static void patch_device_context(uint8_t *ctx) {
    g_last_patched_ctx = ctx;
    void *real_ctx1b8_obj = try_construct_real_ctx_obj(ctx);
    if(real_ctx1b8_obj) {
        log_debug("DIAG: real ctx+0x260 factory (RVA 0x%x) returned a real object: %p", CTX260_CTOR_RVA, real_ctx1b8_obj);
        *(void**) (ctx + 0x1b8) = real_ctx1b8_obj;
    } else {
        log_debug("DIAG: real ctx+0x260 factory returned NULL, falling back to fake object");
        *(void**) (ctx + 0x1b8) = get_fake_ctx_obj();
    }
    *(uint32_t*) (ctx + 0x144) = 1;
    *(uint32_t*) (ctx + 0x148) = 1;
    //Also seen as zero live (passed as the vtable[24] call's 2nd/3rd args) - set nonzero on the
    //chance they gate something; confirmed live this does NOT change the 0x80070070 outcome below,
    //kept only because it's harmless.
    *(uint16_t*) (ctx + 0x137) = 1;
    *(uint16_t*) (ctx + 0x139) = 1;

    //ctx->field_0x100 gates a real polling loop that otherwise short-circuits to 0x80070070 when
    //NULL. tudor_reset_ctx100 (called from device.c before every StartCapture dispatch) should
    //already have cleared it by the time we get here - this check+patch is the fallback. See
    //FINDINGS.md for the full trace and the current open blocker (0x8009800B) past this point.
    if(!*(void**) (ctx + 0x100)) {
        void *real_obj = try_construct_real_field0x100_obj(real_ctx1b8_obj);
        if(real_obj) {
            log_debug("DIAG: real ctx+0x100 factory (RVA 0x%x) returned a real object: %p", FIELD0X100_CTOR_RVA, real_obj);
            *(void**) (ctx + 0x100) = real_obj;
        } else {
            log_debug("DIAG: real ctx+0x100 factory failed, falling back to quine object");
            *(void**) (ctx + 0x100) = get_fake_quine_obj();
        }
    }
}

//A second, independent NULL-dereference of ctx->field_0x1b8 is also hit synchronously during
//enrollment (unrelated to the StartCapture worker thread patched below). A SIGSEGV-based
//catch-and-retry handler was tried and reverted (caused an infinite fault loop) - see FINDINGS.md.

//Called for every real Win32 CreateThread the driver issues; STARTCAPTURE_THREAD_RVA identifies
//the one StartCapture spawns. See FINDINGS.md for the full traced call chain from here down to the
//current open blocker (0x8009800B).
void tudor_hook_thread_create(void *start_proc, void *param) {
    if(!tudor_driver_dll) return;
    uintptr_t rva = (uintptr_t) start_proc - (uintptr_t) tudor_driver_dll->image.base_addr;
    log_debug("DIAG: CreateThread for driver RVA 0x%lx param=%p (StartCapture RVA is 0x%x)", rva, param, STARTCAPTURE_THREAD_RVA);
    if(rva != STARTCAPTURE_THREAD_RVA) return;

    uint8_t *ctx = (uint8_t*) param;
    log_debug("DIAG: pre-patch ctx=%p +0x100=%p +0x168=%p +0x1b8=%p +0x144=%u +0x148=%u +0x137=%u +0x139=%u", ctx,
        *(void**) (ctx + 0x100), *(void**) (ctx + 0x168), *(void**) (ctx + 0x1b8),
        *(uint32_t*) (ctx + 0x144), *(uint32_t*) (ctx + 0x148), *(uint16_t*) (ctx + 0x137), *(uint16_t*) (ctx + 0x139));
    //Since OnPrepareHardware does its real work (RetrieveDeviceName / IWDFPropertyStoreFactory), the
    //driver populates this context itself - patching it now overwrites real state. The legacy patch
    //is kept behind TUDOR_LEGACY_CTX_PATCH for comparison only. See FINDINGS.md.
    g_last_patched_ctx = ctx;
    if(!getenv("TUDOR_LEGACY_CTX_PATCH")) return;
    patch_device_context(ctx);
    log_debug("DIAG: patched StartCapture thread context %p (fields +0x1b8/+0x144/+0x148/+0x137/+0x139), ctx+0x1b8 now %p", ctx, *(void**) (ctx + 0x1b8));
}

#define NUM_WINDRV_DLLS 2
struct windrv_dll tudor_windrv_dlls[] = {
    {
        .module = {
            .name = "synaBscAdapter52.dll",
            .cmdline = "synaBscAdapter52.dll",
            .environ = (const char*[]) { NULL }
        },
        .pe_image = &_binary_libtudor_synaBscAdapter52_dll_start, .pe_image_end = &_binary_libtudor_synaBscAdapter52_dll_end,
        .is_adapter = true, .is_driver = false
    },
    {
        .module = {
            .name = "synaWudfBioUsb52.dll",
            .cmdline = "synaWudfBioUsb52.dll",
            .environ = (const char*[]) { NULL }
        },
        .pe_image = &_binary_libtudor_synaWudfBioUsb52_dll_start, .pe_image_end = &_binary_libtudor_synaWudfBioUsb52_dll_end,
        .is_adapter = false, .is_driver = true
    }
};

bool tudor_log_traces;

static struct winmodule ntdll_module = {
    .name = "ntdll.dll",
    .cmdline = "ntdll.dll",
    .environ = (const char*[]) { NULL }
};

static struct winmodule kernel32_module = {
    .name = "kernel32.dll",
    .cmdline = "kernel32.dll",
    .environ = (const char*[]) { NULL }
};

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
typedef BOOL __winfnc (*api_DllMain)(HANDLE hinstDLL, int fdwReason, void *lpReserved);

struct windrv_dll *tudor_adapter_dll, *tudor_driver_dll;
WINBIO_SENSOR_INTERFACE *tudor_sensor_adapter;
WINBIO_ENGINE_INTERFACE *tudor_engine_adapter;

static DRIVER_OBJECT umdf_driver;
struct winwdf_driver *tudor_wdf_driver;
bool tudor_using_com_activation = false;

bool tudor_init() {
    //Register dummy modules
    winmodule_register(&ntdll_module);
    winmodule_register(&kernel32_module);

    if(tudor_log_traces) {
        //Register trace messages
        winlog_register_trace_msg(DEFINE_GUID(58f95b1a, 8efd, 39f0, 5626, 3e620b587295), 0x0c, "%s<X> checkpoint hit <X>");
        winlog_register_trace_msg(DEFINE_GUID(58f95b1a, 8efd, 39f0, 5626, 3e620b587295), 0x0d, "%s<X> checkpoint hit <X>");

        winlog_register_trace_msg(DEFINE_GUID(824d7f8b, e993, 3db5, 6a1a, 91a0d317b75a), 0x0a, "%s-> %s");
        winlog_register_trace_msg(DEFINE_GUID(824d7f8b, e993, 3db5, 6a1a, 91a0d317b75a), 0x0b, "%s<- %s");
        winlog_register_trace_msg(DEFINE_GUID(824d7f8b, e993, 3db5, 6a1a, 91a0d317b75a), 0x0c, "%s<- %s [0x%x]");
        winlog_register_trace_msg(DEFINE_GUID(824d7f8b, e993, 3db5, 6a1a, 91a0d317b75a), 0x0d, "%s-> %s");
        winlog_register_trace_msg(DEFINE_GUID(824d7f8b, e993, 3db5, 6a1a, 91a0d317b75a), 0x0f, "%s<- %s [0x%x]");

        winlog_register_trace_msg(DEFINE_GUID(2c18840b, 2ee0, 377e, f168, 1552bbd307c4), 0x0a, "VFM LOG | %s\033[1A");
    }

    //Set registry handler
    winreg_set_handler(tudor_reg_handler, NULL);

    //Load driver DLLs
    tudor_adapter_dll = tudor_driver_dll = NULL;
    for(int i = 0; i < NUM_WINDRV_DLLS; i++) {
        struct windrv_dll *dll = &tudor_windrv_dlls[i];
        if(!load_dll(&dll->image, dll->module.name, dll->pe_image, dll->pe_image_end - dll->pe_image)) {
            log_error("Error loading driver DLL!");
            return false;
        }
        winmodule_register(&dll->module);
        log_info("Loaded driver DLL '%s' [%ld bytes]", dll->module.name, dll->pe_image_end - dll->pe_image);

        if(dll->is_adapter) tudor_adapter_dll = dll;
        if(dll->is_driver) tudor_driver_dll = dll;
    }
    if(!tudor_adapter_dll) abort();
    if(!tudor_driver_dll) abort();
    apply_driver_patches(&tudor_driver_dll->image);

    //Initialize driver DLLs
    for(int i = 0; i < NUM_WINDRV_DLLS; i++) {
        struct windrv_dll *dll = &tudor_windrv_dlls[i];

        if(dll->image.entry_point) {
            log_info("Initializing driver DLL '%s'...", dll->module.name);
            winmodule_set_cur(&dll->module);
            if(!((api_DllMain) dll->image.entry_point)(dll->module.handle, DLL_PROCESS_ATTACH, NULL)) {
                log_error("Error initializing driver DLL '%s'!", dll->module.name);
                return false;
            }
        }
    }

    //Call UMDF driver entry function - either the modern (UMDF2/KMDF-compatible) flat function
    //table export, or, if that's not present, fall back to the legacy UMDF 1.x COM-based
    //DllGetClassObject/IDriverEntry activation model.
    init_winwdf();
    winmodule_set_cur(&tudor_driver_dll->module);

    void *fx_driver_entry = find_dll_export_optional(&tudor_driver_dll->image, "FxDriverEntryUm");
    if(fx_driver_entry) {
        char16_t *reg_path_wstr = winstr_from_str("HKEY_LOCAL_MACHINE\\Tudor\\Driver");
        UNICODE_STRING reg_path = {
            .Length = winstr_len(reg_path_wstr)+1,
            .MaximumLength = winstr_len(reg_path_wstr)+1,
            .Buffer = reg_path_wstr
        };

        NTSTATUS status;
        if((status = ((api_FxDriverEntryUm) fx_driver_entry)(&wdf_loader, NULL, &umdf_driver, &reg_path)) != 0) {
            log_error("Error in UMDF driver entry function: 0x%x!", status);
            return false;
        }

        free(reg_path_wstr);

        if(!(tudor_wdf_driver = winwdf_get_driver(&wdf_globals))) {
            log_error("UMDF entry function didn't create a WDF driver!");
            return false;
        }
    } else {
        log_info("No FxDriverEntryUm export, falling back to legacy COM-based driver activation...");
        HANDLE reg_key = winreg_open_key(NULL, "HKEY_LOCAL_MACHINE\\Tudor\\Driver");
        if(!tudor_init_com_driver(&tudor_driver_dll->image, reg_key, &tudor_wdf_driver)) {
            log_error("COM-based driver activation failed!");
            return false;
        }
        tudor_using_com_activation = true;
    }

    //Query WINBIO interfaces
    winmodule_set_cur(&tudor_adapter_dll->module);

    HRESULT hres;
    if((hres = ((api_WbioQuerySensorInterface) find_dll_export(&tudor_adapter_dll->image, "WbioQuerySensorInterface"))(&tudor_sensor_adapter)) != 0) {
        log_error("Error querying sensor interface: 0x%x!", hres);
        return false;
    }
    if((hres = ((api_WbioQueryEngineInterface) find_dll_export(&tudor_adapter_dll->image, "WbioQueryEngineInterface"))(&tudor_engine_adapter)) != 0) {
        log_error("Error querying engine interface: 0x%x!", hres);
        return false;
    }

    log_debug("DIAG: adapter_base=%p StartCapture=%p (RVA 0x%lx) FinishCapture=%p (RVA 0x%lx) PushDataToEngine=%p (RVA 0x%lx)",
        tudor_adapter_dll->image.base_addr,
        (void*) tudor_sensor_adapter->StartCapture, (uint8_t*) tudor_sensor_adapter->StartCapture - (uint8_t*) tudor_adapter_dll->image.base_addr,
        (void*) tudor_sensor_adapter->FinishCapture, (uint8_t*) tudor_sensor_adapter->FinishCapture - (uint8_t*) tudor_adapter_dll->image.base_addr,
        (void*) tudor_sensor_adapter->PushDataToEngine, (uint8_t*) tudor_sensor_adapter->PushDataToEngine - (uint8_t*) tudor_adapter_dll->image.base_addr);

    return true;
}

bool tudor_shutdown() {
    //Unload the driver
    winmodule_set_cur(&tudor_driver_dll->module);

    log_debug("Unloading WDF driver...");
    winwdf_unload_driver(tudor_wdf_driver);
    
    if(umdf_driver.DriverUnload) {
        log_debug("Unloading UMDF driver...");
        umdf_driver.DriverUnload(&umdf_driver);
    }
    umdf_driver = (DRIVER_OBJECT) {0};

    //Uninitialize driver DLLs
    for(int i = 0; i < NUM_WINDRV_DLLS; i++) {
        struct windrv_dll *dll = &tudor_windrv_dlls[i];

        if(dll->image.entry_point) {
            log_info("Uninitializing driver DLL '%s'...", dll->module.name);
            winmodule_set_cur(&dll->module);
            if(!((api_DllMain) dll->image.entry_point)(dll->module.handle, DLL_PROCESS_DETACH, NULL)) {
                log_error("Error uninitializing driver DLL '%s'!", dll->module.name);
                return false;
            }
        }
        winmodule_unregister(&dll->module);
    }

    //Destroy driver DLLs
    for(int i = 0; i < NUM_WINDRV_DLLS; i++) destroy_dll(&tudor_windrv_dlls[i].image);

    //Unregister dummy modules
    winmodule_unregister(&ntdll_module);
    winmodule_unregister(&kernel32_module);

    return true;
}
