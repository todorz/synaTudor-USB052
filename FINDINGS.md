# Investigation Findings

Reverse-engineering notes for getting `synaWudfBioUsb52.dll` / `synaBscAdapter52.dll`
(Synaptics WBDI fingerprint driver, VID:PID 06cb:0082) running under libtudor. This
documents *why* the patches in the code exist, the dead ends already ruled out, and
the current open blocker — so future work doesn't re-walk the same ground.

All RVAs below are relative to `synaWudfBioUsb52.dll`'s image base unless noted
otherwise. ASLR is deterministic in this environment (same live addresses
every run) but the driver's live base **shifts whenever the compiled binary's
own size changes** (e.g. after editing driver.c) - don't assume a specific
base address is still current; recompute it live each session via the
`tudor_hook_thread_create` breakpoint technique in Methodology notes
(`base = $rdi - 0x1c21c`). `RVA = live_addr - live_base`, `static_VA =
0x180000000 + RVA`.

## Current status

**WORKING (2026-10-11): full enroll, identify and verify succeed on the real
06cb:0082 sensor with real finger touches, and the process shuts down cleanly.**
Verified sequence: about 20 enroll captures (progress 0->100%), template
export, "Found match: identity index=1 finger=1", "VERIFICATION SUCCESS",
"exited normally". Cancellation (Ctrl+C / `tudor_cancel_async`) also works.
See the last sections of this file ("The real driver now initializes the
StartCapture context by itself" onward) for what fixed it. Most of the
history below documents workarounds for state the driver now sets up itself.
Those workarounds are now disabled by default (`TUDOR_LEGACY_CTX_PATCH`).

Also verified, in a fresh process loading the saved data store (so
persistence works): identify with the enrolled finger matched, identify with
a different finger returned "no match", verify with the enrolled finger
succeeded, and verify with a different finger failed. The matcher is real,
not a shim artifact. Not yet tested: the libfprint/tudor-host path.

### Historical status (superseded)


**Major milestone reached this session: the entire driver bridge now runs
end-to-end with no crash, hang, or memory corruption**, from process start
through `tudor_open`, the interactive CLI menu, and a full `enroll` attempt
(capture dispatch, real `StartCapture` processing, graceful failure/discard
with no finger present). This required resolving, in order (see "Enrollment
hang" below for the full blow-by-blow): the original enrollment deadlock and
two SIGSEGV classes it exposed; switching `ctx->field_0x100` to the driver's
REAL constructor instead of a fully-fake object; a real layout disagreement
between two pieces of real driver code (worked around with targeted NOPs +
a safe substitute pointer); an over-broad "make these slots callable" patch
that had broken a different consumer; a NULL-source `memcpy` traced to
`GetInputMemory`/`GetOutputMemory` both needing a safe scratch buffer
whenever the real driver requests a zero-size one but reads from it anyway;
and finally, the deepest one - a genuine **use-after-free**: the real driver
actually calls `free()` on this project's fake objects as part of its own
COM-style AddRef/Release lifecycle, and something else still held a live
reference to that same memory afterward. Fixed by making `HeapFree` a
permanent no-op (see "Follow-up: root-caused - glibc tcache safe-linking").

**`0x8009800B` is now fixed** (clearing `ctx->field_0x168` at devctrl completion
time - see "Open blocker" below for the full history). StartCapture now
genuinely dispatches into the driver's real worker thread instead of bouncing
off "already in progress" immediately.

**New, deeper blocker found immediately behind it: the driver's own capture
worker thread never signals its own completion, physical finger touch or
not.** Confirmed live (real finger touch, reported by the user - identical
result whether touched or not) and via automated testing + GDB tracing (see
"Capture worker thread never completes" below). This is the current blocker on
enrollment succeeding.

`identify()` separately reaches real driver capture logic but was previously
untested against this specific blocker - expect the same symptom, since it
goes through the same `StartCapture` worker thread mechanism.

## Background: device-open sequence (`device.c: tudor_open`)

- **`EngineContext+0x8c`**: `synaBscAdapter52.dll`'s own StartCapture (real,
  unmodified) gates real capture work behind `EngineContext+0x8c == 1` — confirmed
  via disassembly (RVA `0x1fd4` in the adapter DLL writes literal `0x70` into the
  completion status when unset, i.e. the same `0x80070070` this project was
  chasing before the fixes above). Meant to be set by a chain of internal
  path/locale CRT logic (`GetModuleFileNameW` → codepage-aware string conversion)
  that never resolves correctly here, even after fixing a real bug in this
  project's own `GetModuleFileNameW` (`winapi/module.c` — it returned a bare
  filename instead of a full path). Patched directly on `EngineContext` (stable at
  this offset regardless of what `Attach()`'s real logic intended it to
  represent) instead of chasing the CRT internals further.
- **Persistent BUSY at open**: this sensor reports `WINBIO_SENSOR_BUSY` at open
  time persistently (20+ second retries never clear it), with no error status —
  only ever this specific code. Since real WinBio consumers only need the sensor
  ready right before a capture (not at open), and this is the officially-matched
  driver for this hardware, a lingering BUSY at open is treated as non-fatal
  (logged, open proceeds; status gets re-checked before capture anyway).
- **Warm-up capture**: the driver's real StartCapture handler spawns a worker
  thread whose device-context object needs the one-time patch in
  `driver.c`'s `tudor_hook_thread_create` applied before its real work runs.
  That patch only fires the first time the specific worker thread is created.
  Without a warm-up, whichever operation happens to run first in a session hits
  it cold — identify tolerates this fine, but enrollment crashes with a NULL
  dereference reading the same unpatched field from a different, synchronous
  code path. Running one throwaway StartCapture immediately after open
  guarantees the patch is in place before any real user-requested operation,
  regardless of which one comes first. Costs ~5s (the devctrl watchdog timeout,
  since nothing is captured with no finger present) per device open.

## Fixed this project (background, not this session unless noted)

- **BUSY status patch** (`driver.c: g_driver_patches`): the driver's device-context
  constructor unconditionally initializes `WINBIO_SENSOR_STATUS` to `BUSY(4)`.
  Extensive tracing (OnPrepareHardware, OnD0Entry, every WINBIO_SENSOR_INTERFACE
  member, every devctrl code, a hardware watchpoint on the status field) found no
  path that ever clears it on this host. Patched directly in the loaded image
  (1-byte immediate, verified against the expected byte before patching) to
  `READY(3)`.
- **StartCapture worker thread context** (`driver.c: patch_device_context`): the
  worker thread the driver spawns via real `CreateThread` (start routine RVA
  `0x1c21c`, confirmed stable) gates its real work behind
  `ctx->field_0x148 != 0 && ctx->field_0x144 != 0` before calling
  `ctx->field_0x1b8->vtable[24]`. All three read as zero/NULL on this host; no
  reachable driver code populates them. Patched directly.
- **`ctx->field_0x1b8` real construction**: rather than a fake stub object, this
  session found and called the real (unmodified) driver factory at RVA `0x16354`
  (takes `ctx+0x260` as its argument) to construct a genuine object here instead.
  Requires `__winfnc` (ms_abi) on the function pointer type — real Windows-ABI code
  takes its first param in RCX, not SysV's RDI. An earlier version of this call
  omitted `__winfnc` and *appeared* to work purely by luck (leftover RCX value from
  preceding real driver code); the bug only became obvious once a second
  ms_abi-missing call was added nearby and crashed outright.
- **Devctrl watchdog use-after-free** (`device.c`): the 5-second force-completion
  watchdog for devctrl requests could fire after the request object had already
  been completed and freed, causing `pthread_mutex_lock(&req->lock)` to fail with
  EINVAL and abort. Fixed by replacing the blind `usleep` with a cancellable
  `pthread_cond_timedwait`, cancelled from `tudor_cleanup` before the request
  object is actually destroyed.
- **Follow-up, same symptom recurred later**: that first fix still had a genuine
  TOCTOU race - `devctrl_watchdog_cancel` searched the shared watchdog list and,
  if the watchdog thread had *just* unlinked itself (about to call
  `WdfRequestComplete` but hadn't yet), found nothing and returned immediately,
  letting `tudor_cleanup` destroy the request while that call was still in
  flight. Reproduced live with the exact same `pthread_mutex_lock(&req->lock)`
  EINVAL abort, this time while testing with the watchdog timeout temporarily
  lengthened (see "Capture worker thread never completes" below) - the race
  existed at any timeout length, just far more likely to hit within a single
  session once there was more time for other work to interleave. Fixed by
  collapsing the per-context lock/cond into the single shared list lock/cond:
  a watchdog now holds that one lock across its *entire*
  decide-then-unlink-then-fire sequence, so `devctrl_watchdog_cancel` either
  sets `cancelled` before that sequence starts, or blocks until the sequence -
  including the `WdfRequestComplete` call itself - has fully finished before it
  can even search the list. Either way, `tudor_cleanup`'s destroy (which always
  runs right after `devctrl_watchdog_cancel` returns) is now safe. Confirmed via
  automated (fingerless) regression testing post-fix: normal enroll-fails-
  gracefully flow still works, no crash.

## Dead ends ruled out (don't re-investigate these)

- **WinUsb.dll**: the driver resolves the full `WinUsb_*` function set via
  `LoadLibraryA("WINUSB.DLL")` + `GetProcAddress` at module-init time, but
  `WinUsb_Initialize` is never actually called, in any tested flow (open, identify,
  enroll, verify) — confirmed via a live breakpoint on our own `WinUsb_Initialize`
  hook, both before and after this session's other fixes.
- **WDF USB target** (`winapi/wdf/usb.c`): a complete, correct implementation
  (`WdfUsbTargetDeviceCreate`/`GetInterface`/pipe read-write, backed by libusb) —
  the standard, Microsoft-documented mechanism WBDI drivers are supposed to use.
  Confirmed via diagnostic logging on every entry point: none of them fire, ever.
- **Default IO target** (`com_get_default_io_target`): implemented, confirmed live
  to never be called.
- **`ctx->field_0x1b8` vtable slot #24**: this project's earlier fake-object
  approach had this slot called live during every StartCapture. Calling the real
  driver factory (see above) instead and inspecting the resulting *real* vtable
  showed slot 24 is a thunk to RVA `0x3e6f4`, whose entire body in the unmodified
  vendor DLL is `xor eax,eax; ret` — a permanent no-op. Not the missing "push"
  mechanism. (An earlier pass through this investigation misread past this
  function's `ret` into the next, unrelated function's bytes and mistook an
  unrelated error path there for slot 24's own logic — that was wrong.)
- **`ctx->field_0x168` "capture queue" theory**: an earlier session pass
  mischaracterized the check `if(ctx->field_0x168 != 0 && ctx->field_0x290 == 0)`
  (RVA `0x1bb94`) as querying a driver-internal hardware result queue, with vtable
  slot 16 as its "pop". Wrong: `ctx->field_0x168` is this project's *own* devctrl
  request facade (`com_activate.c`'s `req_facade`), and slot 16 there is
  `com_request_get_devctrl_params`, one of this project's own already-working COM
  methods — not driver-internal queue logic at all. (`ctx->field_0x168` does turn
  out to matter for a different reason — see "Open blocker" below.)
- **Registry values**: only two registry reads happen during a capture attempt
  (`evTraceEnableLevel`, `evlogTypes`), both ETW/tracing config, both expected to
  fail harmlessly (this project has no ETW infrastructure). Not related to capture.
- **`MarkCancelable`/`UnmarkCancelable`** (req_facade vtable slots 19/20): called
  during every StartCapture devctrl, currently handled by the generic
  logging-stub-returns-S_OK fallback. Since a "real" implementation would also
  just return S_OK, implementing these explicitly doesn't change behavior — not
  the blocker.
- **`cleanup_real_ctx100`** (RVA `0x1c268`, the real "cancel/release active capture
  object" routine): calling this real driver function on our fake
  `ctx->field_0x100` object hangs the process indefinitely (confirmed live,
  twice). Its cleanup path evidently does more than a plain `Release()` when it
  thinks a capture is genuinely in progress (a `ctx->field_0xec==1` check in its
  own disassembly likely leads into real cancellation/synchronization logic that
  blocks waiting on something a fake object will never provide). This project
  clears the field directly instead (`tudor_reset_ctx100`).

## The `0x80070070` chain (mechanically understood, fixed this session)

Traced the full call chain live from the StartCapture worker thread down to the
literal source of `HRESULT_FROM_WIN32(112)` = `0x80070070`:

1. Gate at RVA `0x1bb94`: `ctx->field_0x168 != 0 && ctx->field_0x290 == 0` — always
   satisfied (0x168 is our own req_facade, always non-NULL; 0x290 stays 0).
2. RVA `0x1bc0a`-`0x1bc3d`: retrieves devctrl params + input memory via our own COM
   facade — confirmed working correctly (verified via DIAG logging: returns a
   valid, non-NULL 32-byte input buffer every time).
3. RVA `0x1bc4e`-`0x1bc65`: buffer-size check against a fixed constant
   (`0x1b8e0` bytes) — passes, our buffer is large enough.
4. RVA `0x1bc89`: reads a 32-bit "raw code" — confirmed live, always exactly 112 —
   and dispatches: 0/`0x67`/`0xcb`/`0x194` get dedicated handling, anything else
   (112 included) falls to a default case.
5. RVA `0x1bdac`: calls `0x180015e74` (confirmed a textbook `HRESULT_FROM_WIN32`
   implementation) on 112, producing `0x80070070` mechanically — not hardcoded as
   a literal anywhere (searched both DLLs for the raw bytes).
6. Completed via RVA `0x1c39c` → this project's own `Complete()`.

Raw code 112 traced to its origin: a large, stack-canary-protected function (RVA
`0x1800125dc`) dispatches on a "purpose" value at `ctx+0x80` into timeout-configured
polling loops (confirmed via a real `GetTickCount`-based elapsed-time check — a
genuine timed poll, not queue logic). Before that dispatch, at RVA `0x1800127c2`:
`cmp QWORD PTR [obj+0x20],0x0` — if zero, RVA `0x1800127cd` immediately writes
literal `0x70` (112) and returns, skipping all real capture logic. `obj+0x20` was
traced (forward from its construction, not backward from this check — backward
tracing repeatedly proved unreliable) to be a direct copy of `ctx->field_0x100`,
made by a real constructor at RVA `0x10e38` called early in the worker thread
(RVA `0x1ae65`).

**Fix**: patch `ctx->field_0x100` to a fake object when NULL (`patch_device_context`
in `driver.c`). A plain vtable-only fake object crashed here — this field's
consumer reads raw fields directly rather than going through a vtable — so a
self-referential "quine" object is used instead (`get_fake_quine_obj`): every
8-byte slot points back at the block's own start, so any short chain of
`dereference, add offset, dereference again` safely resolves back into the same
zeroed memory rather than wandering into unmapped memory.

This gets past `0x80070070` with no crash, confirmed live, both with and without a
real finger on the sensor (the no-finger case behaves identically — ruling out the
theory that `0x80070070` might be an *expected* "nothing to report" answer rather
than a bug).

## Open blocker: `0x8009800B` (`WINBIO_E_DATA_COLLECTION_IN_PROGRESS`)

Getting past `0x80070070` reveals the next failure: every StartCapture attempt now
fails immediately with a **hardcoded literal** `0x8009800B` (`mov edx,0x8009800b`
at RVA `0x1f6d6`, confirmed via disassembly — this one genuinely is a fixed
constant, unlike `0x80070070`).

**First cause found and fixed**: leaving the fake `ctx->field_0x100` object in
place forever (after patching it once) makes every subsequent capture look like an
operation is already running — self-consistent with `field_0x100` genuinely being
a per-capture "active operation" handle. Fixed by clearing it before every
StartCapture dispatch (`tudor_reset_ctx100`, called from `device.c`'s
`tudor_devctrl` for code `0x440014`, and after the warm-up capture in
`tudor_open`). Confirmed live via diagnostic logging that the field *is* clear
immediately before every dispatch.

**But `0x8009800B` still fires every time regardless.** Re-traced its origin fresh
under this fixed state: same literal-constant write site (RVA `0x1f6d6`), but a
different, earlier real gate controls whether that code is even reached. A
function starting around RVA `0x1f4f4` checks, twice, `ctx->field_0x168 != 0`
(RVA `0x1f579` and `0x1f5d9`) OR `ctx->field_0x198 != 0` (the real
`CreateThread`-returned thread HANDLE, stored by the synchronous dispatcher at RVA
`0x27fb0`) — if either is set, a flag gets set that leads into the `0x8009800B`
write further down; if *both* are clear, execution jumps past all of it (RVA
`0x1f5f6`: `je 0x1f70f`) to what's presumably real capture logic instead.

`ctx->field_0x168` is this project's own devctrl request-facade pointer — the
*same* field checked all the way back at the very first gate this project found
this session (RVA `0x1bb94`'s `ctx->field_0x168 != 0`). This strongly suggests
`field_0x168` (not `field_0x100`, or not only it) is what actually needs resetting
between attempts.

### Update: `ctx->field_0x168` confirmed non-NULL, but not safely clearable

Live-checked `ctx->field_0x168` at the actual gate (correcting a transcription
error: the real check instruction is at RVA `0x1f57a`, not `0x1f579` — objdump had
split one 8-byte `mov` across two printed lines, and the "check" address I'd copied
was actually the trailing byte of the *previous* instruction, which is why a first
attempt at breaking there never fired). At the correct address, `ctx->field_0x168`
is confirmed non-NULL (`0x5555555ac480`, matching this project's own live
req_facade for the *current* request) — so the `jne` branch is taken and
`0x8009800B` fires every time, as expected.

Found this field's real driver-side cleanup path too: RVA `0x1f33b` (inside a block
at RVA `0x1f200`-`0x1f346` that completes whatever `ctx->field_0x168` points to via
`Complete()`, then clears it) — confirmed live to never run, same class of gap as
`field_0x100`'s cleanup.

**Two attempts to clear it directly were both tried live and both made things
worse, not better:**

- Clearing it in `tudor_reset_ctx100` *before* the StartCapture dispatch (same
  timing as the working `field_0x100` fix) **hung the process indefinitely**
  (confirmed live, 60s and 180s timeouts). The caller chain traced this session
  shows RVA `0x1f4f4` (the `0x8009800B` check function) is called *synchronously*,
  from the driver's own top-level devctrl-code dispatch switch (RVA `0x37a00`
  onward, confirmed via its literal IOCTL-code comparisons e.g. `0x442014`) —
  earlier in the driver's own processing than `field_0x100` is ever touched. Since
  `field_0x168` is also this project's own req_facade (needed correctly set for
  `GetDeviceIoControlParameters`/`GetInputMemory` etc. within the *same* devctrl
  callback), clearing it this early likely leaves the driver's own internal state
  machine waiting on something that depends on it already being valid.
- Clearing it *after* the StartCapture request completes (via a dedicated
  completion callback, `req_cb_startcapture`, mirroring the real driver's own
  "complete then clear" ordering) avoided the hang, but instead crashed with
  `Fatal glibc error: tpp.c:83 (__pthread_tpp_change_priority): assertion failed`
  — a pthread priority-inheritance mutex corruption symptom, not obviously
  related to this field directly, but reproduced consistently right after this
  change. Not root-caused given time spent; reverted along with the hang-causing
  version.

Both attempts are reverted; the codebase is back to the stable `field_0x100`-only
fix (no crash, no hang, `0x8009800B` every time).

### Update: the `0x1f33b` cleanup path is a virtual method, not called directly

Traced why RVA `0x1f33b`'s cleanup never runs: it's inside a function starting at
RVA `0x1f160` that takes `(ctx, expected_value)` and only completes+clears
`ctx->field_0x168` if it still equals `expected_value` (a compare-and-clear guard
against clearing a newer request that's since overwritten the field - RVA
`0x1f1b8`: `cmp QWORD PTR [rax+0x168],rcx / jne <skip>`). This function has **no
direct callers anywhere in the DLL** (searched via `call 0x...` cross-reference,
same method used successfully elsewhere this session) - it's invoked *indirectly*,
through a vtable: its address (`0x18001f160`) appears as slot 3 (0-indexed, offset
`0x18`) in three distinct-but-related vtables in `.rdata` (file offsets
`0x12cc28`, `0x12cec8`, `0x18012e5c0` roughly - all three share the same tail
entries `0x1800160e8, 0x1800160e8, 0x1800223a8`, suggesting a family of related
C++ classes, possibly sharing a common base). Searching for the constructor sites
that assign these vtables (`lea reg,[rip+offset]` targeting those addresses) came
up empty - they may be assigned via relocations/data initialization rather than
visible `lea` code, which this session's `objdump`+`grep` methodology can't easily
find. Around 21 candidate `call QWORD PTR [reg+0x18]` sites exist DLL-wide as
possible callers of *some* vtable's slot 3, not yet individually checked live.

### Update: resolved — clear `ctx->field_0x168` at devctrl completion time

The one untried variant from the two attempts above: clear `ctx->field_0x168`
directly at the natural completion point, i.e. inside `com_activate.c`'s
`com_request_complete`/`com_request_complete_with_information` (right after their
`WdfRequestComplete` calls), rather than before dispatch (hung) or via a separate
delayed callback (crashed with a pthread priority assertion). New function
`tudor_reset_ctx168()` in `driver.c`, called from both completion functions.

Confirmed live via automated (no real finger) testing: `0x8009800B` no longer
fires. StartCapture now genuinely starts, reaches "Put your finger on the sensor",
and — with no finger present — times out via the existing devctrl watchdog with
`0xc00000b5` (`STATUS_IO_TIMEOUT`) instead, followed by a clean
`Discarding enrollment of guid=...` and a normal return to the menu. No crash, no
hang. This is the same graceful "nothing to report" shape already established for
the fingerless test path, just one step further down the pipeline than before.

The earlier crash from the "clear after completion via dedicated callback" attempt
did not reproduce here — plausibly it depended on instability fixed later in this
session (the `HeapFree` use-after-free root cause, see above), consistent with the
hypothesis that motivated retrying this variant.

**Not yet confirmed**: a real physical finger touch actually completing enrollment
successfully (as opposed to the no-finger case timing out gracefully). That's the
next thing to test.

### Next step

`field_0x168` is confirmed to be the blocker, its real cleanup mechanism is now
understood in principle (compare-and-clear, called virtually) but its actual call
site hasn't been found. Two directions worth trying next:
- Check the ~21 candidate `call [reg+0x18]` sites live (breakpoint each, check
  whether the object's own vtable pointer matches one of the three found) to find
  which one is the real caller, then see what triggers it (and why it doesn't fire
  in this bridge).
- Alternative, likely faster: reconsider whether `field_0x168` is actually the
  *right* field to change at all - since it's legitimately this project's own
  active-request facade during the current callback, forcing it to read as "not
  set" here may be fighting the driver's real design rather than fixing a genuine
  gap. The two direct-clear attempts already tried (both reverted - see above)
  support this: both made things *worse*, which is a signal this field's
  lifecycle is more load-bearing elsewhere than assumed.

## Capture worker thread never completes (current blocker, behind `0x8009800B`)

With `0x8009800B` fixed, StartCapture (devctrl `0x440014`) genuinely dispatches:
the driver spawns its real worker thread at RVA `0x1c21c` (same thread routine
already tracked elsewhere in this doc as `STARTCAPTURE_THREAD_RVA`). **This
worker thread never completes, confirmed with a real physical finger touch
(user-reported, identical result touched or not) and independently via live GDB
tracing with no finger present** - same failure either way, which itself is a
strong signal the sensor's physical state isn't reaching the code that would
act on it.

### What the worker thread actually does (live-traced)

Live-traced via a GDB watchpoint on `WaitForSingleObject` (this project's own
implementation, `winapi/sync.c`) plus a live SIGINT-and-backtrace against a
running capture attempt. Per attempt:

1. Two auto-reset events are created (`CreateEventA(manual_reset=0,
   initial_state=0)`, confirmed live, always exactly two per worker-thread
   launch).
2. The thread touches this project's fake `ctx->field_0x1b8` object (see
   "Background" above) at vtable slots #2 and #4 a few times each - **confirmed
   via disassembly this is WPP/ETW software-tracing boilerplate, not
   functional code**: the args pattern (`0x938ae058`/`0x938ae060`/`0x938ae064`
   - small sequential values, not pointers) matches a WPP event-descriptor ID,
   and the surrounding disassembly (RVA `0x14ccc` area) is a textbook
   WPP-generated "if tracing enabled, format and log an event" wrapper -
   `evTraceEnableLevel`'s registry read failing (see "Dead ends ruled out")
   makes the enabled-check false, so this is normally a no-op in real Windows
   too. Not the blocker.
3. The thread calls `WaitForSingleObject(handle, INFINITE)` on one of the two
   events from step 1 (RVA `0x14e41`/`0x14e45`, confirmed via `objdump`:
   `mov rcx,[rax+0x28]` / `call [rip+...]` resolving to the `WaitForSingleObject`
   IAT thunk at RVA `0x2d5a8`) - **and never returns**. Confirmed live: this
   thread (a fresh LWP per capture attempt) sits in `pthread_cond_wait` inside
   this project's `evt_wait`/`win_wait_sync_obj`/`WaitForSingleObject` chain for
   the rest of the process's life. Each capture attempt leaks one such thread -
   this project's devctrl watchdog (see "Fixed this project") force-completes
   the *outer* IRP after 5s regardless, which is why the CLI still returns to
   the menu, but the real driver-side worker never itself finishes or retries.

### What was ruled out as the missing trigger

Exhaustively checked - **zero hits across an entire enroll attempt, real
finger touch or not** - for every real I/O mechanism this driver could
plausibly use to talk to the sensor before signaling that event:

- `WinUsb_ReadPipe`/`WinUsb_WritePipe` (`winapi/winusb.c`) - zero calls (matches
  the pre-existing "Dead ends ruled out" finding that `WinUsb_Initialize`
  itself is never called either).
- WDF USB target pipe read/write (`winapi/wdf/usb.c`) - zero calls (matches the
  pre-existing dead-end finding for this whole file).
- `DeviceIoControl` (`winapi/io.c`) - the implementation logs unconditionally
  for *every* handle, not just this project's own virtual WDF device; zero
  calls beyond the already-known `0x440008`/`0x440010`/`0x440014`/`0x44200c`/
  `0x44201c` sequence to this project's own device.
- `CreateFileA`/`CreateFileW` (`winapi/setupapi.c`) - zero calls. Rules out the
  driver opening some *other* raw device path (e.g. a second WinUSB-class
  interface reached by explicit `IOCTL_WINUSB_*` codes bypassing `WINUSB.DLL`'s
  exported functions) - there's no second handle to issue those IOCTLs against.
- `SetEvent` (this project's own `winapi/sync.c`) - only 2 calls total across a
  full enroll attempt (both attempts, both devctrl 0x44200c/0x44201c "arm" and
  "discard" transitions), and both fire as part of the *next* devctrl's own
  cleanup path, releasing the *previous*, already-abandoned attempt's context -
  never as a "capture complete, data ready" signal fired *during* the capture
  window itself.

This means: as far as this project has instrumented, **there is currently no
reachable code path, anywhere, that would issue real hardware I/O to the
sensor for a capture, regardless of physical finger state.** Not a missing
plumbing detail on this project's side of a call that's happening - the real
driver code itself never appears to attempt hardware I/O at all before parking
in the wait. The actual trigger mechanism (what's supposed to make something
call `SetEvent` on that handle once real data is ready) hasn't been found yet.

### Open questions / next steps

- The call chain from the worker thread's entry (RVA `0x1c21c`) down to the
  `WaitForSingleObject` at RVA `0x14e45` was **not** found via a direct `call`
  in the disassembly (searched the whole RVA range in between) - it must be
  reached through one of several indirect (`call QWORD PTR [reg+offset]`)
  calls seen in the thread routine's disassembly (offsets `0x8`/`0x48`/`0x50`/
  `0x78`/`0x80`/`0xa0`/`0xd0` from various base registers - NOT the same
  offsets as the confirmed-logged `ctx->field_0x1b8` calls at `0x10`/`0x20`,
  so these are dispatching through a *different* object, likely the real
  (unpatched) `ctx->field_0x100` or `ctx->field_0x260` objects this project
  already constructs via the driver's own real factories). Live tracing
  (breakpoints on each candidate indirect call site, checking the vtable
  pointer actually dispatched through) would identify which real driver method
  this is and might reveal what it's *supposed* to trigger.
- Raw frame-pointer-less backtraces beyond the immediate return address are
  unreliable here (confirmed live: frame #2 in one capture resolved to an
  address *below* the driver's own load base, i.e. garbage) - don't trust
  anything past frame #1 without independently verifying it lands just after a
  `call` instruction in the disassembly.
- Worth checking whether this specific sensor (06cb:0082) is a "match-in-sensor"
  device that expects a fundamentally different communication pattern (vendor
  control transfers / interrupt-endpoint polling for a match result) rather
  than a bulk image pull - would explain why none of the standard bulk-read
  mechanisms are ever engaged. Not yet investigated.
- Every capture attempt currently leaks one permanently-blocked pthread - fine
  for short CLI test runs (matches the existing `HeapFree` leak-is-fine
  tradeoff) but worth keeping in mind if this ever needs to run unattended for
  a long session.

### The documented UMDF 1.x USB access path - checked, and not what's happening here

Researched the real, Microsoft-documented mechanism a UMDF 1.x COM-activated
driver is supposed to use for USB access (this fork's driver uses that legacy
activation model - see the top of `com_activate.c` - unlike upstream
[Popax21/synaTudor](https://github.com/Popax21/synaTudor), whose flat
`WDF_FUNCTION_TABLE` drivers really do talk to `WinUsb_ReadPipe` directly, per
its own `device.c`, confirmed by reading its source - that's *why* upstream's
mechanism doesn't apply here and had to be bridged from scratch for this fork:
[Working with USB Devices in UMDF 1.x drivers](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/wdf/working-with-usb-devices-in-umdf-1-x-drivers.md)):
the driver calls `pDevice->QueryInterface(IID_IWDFUsbTargetFactory, ...)`
(typically from inside `IPnpCallbackHardware::OnPrepareHardware`), then
`IWDFUsbTargetFactory::CreateUsbTargetDevice`, then
`IWDFUsbTargetDevice::RetrieveUsbInterface` /
`IWDFUsbInterface::RetrieveUsbPipeObject` to get a pipe, then
`IWDFUsbTargetPipe::FormatRequestForRead` + send (or
`IWDFUsbTargetPipe2::ConfigureContinuousReader` for an interrupt-style
continuous reader).

Got the **real vtable order** for this whole interface family (not just
alphabetical docs, which the "Follow-up" section below already found to be
misleading) from the actual WDK header source, `wudfusb.h`
([mirror](https://github.com/wmliang/wdk-10/blob/master/Include/wdf/umdf/1.11/wudfusb.h)):

- `IWDFUsbTargetFactory` (IID `3f7becf9-3a65-4348-a4f3-339d5734a9c6`): slots
  0-2 IUnknown, slot 3 `CreateUsbTargetDevice`.
- `IWDFUsbTargetDevice` (IID `4cd12e96-900a-44c3-a1b7-05b8954dab76`): slots 0-2
  IUnknown, 3 `DeleteWdfObject`, 4 `AssignContext`, 5 `RetrieveContext`, 6
  `AcquireLock`, 7 `ReleaseLock`, 8 `GetTargetFile`, 9
  `CancelSentRequestsForFile`, 10 `FormatRequestForRead`, 11
  `FormatRequestForWrite`, 12 `FormatRequestForIoctl`, 13 `GetWinUsbHandle`, 14
  `GetNumInterfaces`, 15 `RetrieveUsbInterface`, 16
  `FormatRequestForControlTransfer`, 17 `RetrieveDeviceInformation`, 18
  `RetrieveDescriptor`, 19 `RetrievePowerPolicy`, 20 `SetPowerPolicy`.
- `IWDFUsbInterface` (IID `a6714a4d-c764-498f-b6d2-032f10ca4e36`): slots 0-2
  IUnknown, 3 `DeleteWdfObject`, 4 `AssignContext`, 5 `RetrieveContext`, 6
  `AcquireLock`, 7 `ReleaseLock`, 8 `GetInterfaceDescriptor`, 9
  `GetInterfaceNumber`, 10 `GetNumEndPoints`, 11 `GetConfiguredSettingIndex`,
  12 `SelectSetting`, 13 `GetWinUsbHandle`, 14 `RetrieveUsbPipeObject`.
- `IWDFUsbTargetPipe` (IID `f373216e-8c42-4c5c-95b4-d2a40b99c0fa`): slots 0-2
  IUnknown, 3 `DeleteWdfObject`, 4 `AssignContext`, 5 `RetrieveContext`, 6
  `AcquireLock`, 7 `ReleaseLock`, 8 `GetTargetFile`, 9
  `CancelSentRequestsForFile`, 10 `FormatRequestForRead`, 11
  `FormatRequestForWrite`, 12 `FormatRequestForIoctl`, 13 `Abort`, 14 `Reset`,
  15 `Flush`, 16 `GetInformation`, 17 `IsInEndPoint`, 18 `IsOutEndPoint`, 19
  `GetType`, 20 `RetrievePipePolicy`, 21 `SetPipePolicy`.
- `IWDFUsbTargetPipe2` (IID `8efe1ab4-f4a2-42bb-9b2b-bbada19d8b80`): same as
  `IWDFUsbTargetPipe` plus slot 22 `ConfigureContinuousReader` (the interrupt-
  style push-based read mechanism, with completion callbacks
  `IUsbTargetPipeContinuousReaderCallbackReadComplete`/`...ReadersFailed`).

**But live-traced (breakpoint on this project's own `com_query_interface`,
logging every call's IID, full device open through a failed enroll attempt)
and confirmed: this driver never calls `QueryInterface` for
`IID_IWDFUsbTargetFactory` (or any other untagged IID) against this project's
device facade, anywhere.** Only one `QueryInterface` call happens in the
entire traced run, for the already-known, already-tagged `IWDFDevice3`. Note
this trace can only see calls made *against this project's own COM facades* -
`OnPrepareHardware`/`OnD0Entry` themselves are invoked via `QueryInterface` on
the *driver's own* callback object (`com_activate.c`'s
`g_pending_pnp_callback_iface`), which is real driver-owned memory invisible
to this instrumentation; only calls the driver makes back *into* one of this
project's facades (like the device object passed into `OnPrepareHardware`)
would show up here, and none matching `IWDFUsbTargetFactory` ever did, despite
neither `OnPrepareHardware` nor `OnD0Entry` logging a failure.

So either this specific driver build acquires its USB target through some
non-standard path this project hasn't identified, or something gates the
driver from ever attempting the standard path in the first place (executes
`OnPrepareHardware` far enough to return success without actually doing the
USB target setup its real Windows counterpart would - similar in spirit to the
`EngineContext+0x8c` gate story). Not yet resolved. If a future pass finds the
real call site, the vtable layouts above are ready to implement against
(bridge each method to the equivalent already-working libusb logic in
`winapi/wdf/usb.c`, which implements the *flat-table* equivalent of this same
functionality and can likely be adapted directly).

### Live-traced real call chain from thread entry toward the wait

Traced live via GDB (`nexti` walk with `scheduler-locking on` - required, since
without it the *other* threads free-run during single-stepping and flood the
log; also required breaking the walk out of the `Breakpoint.stop()` callback
into a separate top-level `python` block after `run` returns, since issuing
step commands from inside the callback hits the same "Cannot execute this
command while the selected thread is running" error noted elsewhere in this
doc). Confirmed the real call chain, several levels deep, from the worker
thread's entry down toward the wait - **not reachable via a simple static
`call` search from the entry function**, because it's nested through several
indirect/nested calls:

1. RVA `0x1c21c` (thread entry) calls RVA `0x1a9f0` directly (`call
   0x18001a9f0`, confirmed live at step 10 of the walk).
2. RVA `0x1a9f0` checks `ctx->field_0x168` (zero → early-out through a fast
   path that sets `ctx->field_0x15c = 3` and returns - this is the *no
   previous request pending* case); when non-zero (the normal case for an
   active devctrl), copies 0x3b bytes from `ctx+0x108`, calls a real method on
   the real `ctx->field_0x260` object (RVA `0x16354`'s construction target -
   see "Background" above), then calls **RVA `0xe38`* offset `0x180010e38`**
   passing **`ctx->field_0x100` directly as an argument** (this project's own
   real-constructed object, see `try_construct_real_field0x100_obj` in
   `driver.c`).
3. RVA `0x180010e38` is a **constructor** for a new "capture operation" object
   (allocated on the caller's own stack, not heap): stores `ctx->field_0x100`
   at the new object's offset `+0x20`, then calls the real `CreateEventA`
   import (`args all zero/NULL` - matches the "two auto-reset events created
   per attempt" already observed) and stores the returned handle at the new
   object's offset **`+0x28`** - **this is the exact handle
   `WaitForSingleObject` later blocks on** (confirmed by offset match against
   the disassembly documented above, RVA `0x14e41`'s `mov rcx,[rax+0x28]`).
   Also initializes a lock at `+0x48`.
4. **Correction, live-disproven**: an earlier pass through this section
   speculated that RVA `0x1a9f0` next calls into a chain
   (`0x18001117c`→`0x18001127c`→`0x18001131c`→`0x180047fe0`→`0x180056c30`)
   reached by picking functions merely *adjacent in the binary* to the
   constructor call, without confirming any of them are actually invoked.
   **A live breakpoint on `0x180056c30` during a full real enroll attempt got
   zero hits** - that whole chain is not part of the real path. Static replay
   of the actual next instructions after the constructor call shows it's
   simpler than that: `0x1a9f0` calls a **trivial setter**, RVA `0x180027f94`
   (`ctx->field_0xf8 = &the_new_op_object; ret` - four instructions, nothing
   more), storing the just-constructed operation object for later reference.
5. Immediately *after* that trivial setter in the binary (not necessarily
   called from the same place - not yet confirmed live) sits a much larger,
   more consequential-looking function, **RVA `0x180027fb0`**: it reads
   `ctx->field_0x168`/`0x198`/`0x290`/`0x280` (the same fields central to the
   `0x8009800B` investigation above) as preconditions, and - if they pass -
   calls the real `CreateThread` import with **RVA `0x1c21c` (this project's
   own already-tracked `STARTCAPTURE_THREAD_RVA`) as the thread start
   routine**. This looks like the *real, top-level StartCapture dispatcher* -
   the function that decides whether to spawn the worker thread at all - not
   a completion-path detail. Not yet live-confirmed as part of the executed
   path (unlike steps 1-3 above, which were).

**Next concrete step, not yet done**: live-breakpoint `0x180027fb0` itself
(entry args, which branch it takes through the `field_0x168`/`0x198`/`0x290`/
`0x280` checks, and whether it's called once per attempt as expected) to
confirm it's genuinely the dispatcher and see which precondition (if any) is
failing. Given how directly it reads the exact fields this project's
`0x8009800B` fix already manipulates, this is a stronger lead than the
disproven chain above - but unconfirmed until traced live. General lesson
reinforced here: **don't trust "the next function in address order" as "the
next function in the call chain"** without confirming the actual call
instruction or a live hit - this cost real time this session.

### Update: `0x180027fb0` confirmed live - and it fires more than once per attempt

Live-breakpointed `0x180027fb0` directly (entry args + the four gating fields
+ a backtrace, every hit, across a single real enroll attempt with no finger
present). Confirmed it *is* reached, but the picture is more complex than
"one dispatcher call, one worker thread, one wait":

- **3 hits on `0x180027fb0` recorded for what should be 2 external
  `StartCapture` devctrl requests** (1 warmup in `tudor_open` + 1 real
  enrollment attempt) - the breakpoint only arms partway through the warmup
  call (needs `tudor_hook_thread_create` to fire first to compute the live
  base, and that itself only fires *from inside* a `0x180027fb0` invocation's
  own `CreateThread` call - so the warmup's own dispatch is inherently
  invisible to this instrumentation, arriving already in-progress). All 3
  captured hits belong to later requests.
- Two of the three hits share the *same* `ctx->field_0x168` value (same
  req_facade), meaning **`0x180027fb0` is called more than once for a single
  external devctrl request** - not a one-shot dispatcher.
- Corroborated independently: this same run's plain devctrl-level log shows
  **4** `"CreateThread for driver RVA 0x1c21c"` lines for only **2**
  `DeviceIoControl code=0x440014` calls - i.e. **on average two worker
  threads get spawned (and leaked, per the existing devctrl-watchdog design)
  per single external StartCapture request**, not one. This was not
  previously known and revises the "leaks one thread per attempt" statement
  made earlier in this doc (should read "leaks *at least* one, likely two").
- All three hits read the four gate fields identically shaped
  (`field_0x198=0`, `field_0x290=0`, `field_0x280=1` in every hit - only
  `field_0x168` differs, and only across the two distinct req_facades, not
  between the two same-facade hits) - so whatever causes the *second* call
  with the same facade isn't visible in these four fields; it's driven by
  something else entirely (a genuine internal retry the driver performs on
  its own, or two logically-different call sites both routing through this
  same function for the same request - not yet distinguished).

**Not yet done**: confirming (a) whether *both* worker threads spawned per
attempt independently reach the same `WaitForSingleObject` stall, or diverge;
(b) what specifically triggers the second `0x180027fb0` call for the same
facade - a breakpoint on the actual `CreateThread` call site *inside*
`0x180027fb0` (RVA `0x280f5`) plus a full backtrace at each hit would show
whether both dispatcher invocations come from the same caller or two
different call sites elsewhere in the driver.

### Conclusive test: extending the watchdog to 60s with a real sustained finger touch

Also checked the real-vtable-order research angle from the WDK header source
directly (`wudfusb.h`, real declaration order, not alphabetical docs): the
*only* documented UMDF 1.x mechanism for USB target acquisition is
`IWDFDevice::QueryInterface(IID_IWDFUsbTargetFactory)` →
`CreateUsbTargetDevice`. Confirmed via `IWDFDevice`/`2`/`3`'s real vtable
order (same header source) that there is no alternative *direct* method on
the device interface itself - no `CreateUsbDevice`-style shortcut exists to
have missed. Combined with the earlier finding that this `QueryInterface`
call never happens anywhere in the traced flow, this rules out the last
"maybe we're just missing a call site" explanation for *how* real USB access
would even begin.

To settle the remaining open question directly - does the driver's own wait
ever complete on its own given enough real time, independent of whatever
mechanism would normally signal it - the devctrl watchdog (`device.c`,
`deadline.tv_sec += 5`) was temporarily bumped to 60 seconds, rebuilt, and
tested with a real physical finger held continuously on the sensor for up to
60 seconds during a real enroll attempt (not a quick touch).

**Result: still force-completed via this project's own watchdog at
`0xc00000b5` (`STATUS_IO_TIMEOUT`), not completed naturally by the driver
itself.** A full 60 seconds of genuine, sustained physical contact with the
sensor made no observable difference - the outcome is identical to the
fingerless automated tests. This is strong, direct confirmation (not just
static/live-trace inference) that **nothing in this bridge's reachable code
will ever complete this operation, regardless of physical sensor state or
elapsed time** - it's not a matter of the driver needing more time or a more
deliberate touch; the actual signal path is simply absent. The watchdog was
reverted back to 5s afterward (the 60s value also seriously slowed down
`tudor_open`'s own warm-up capture, which hits the identical stall on every
single process start).

**This test also exposed a real, unrelated bug** - see "Devctrl watchdog
use-after-free" → "Follow-up, same symptom recurred later" above (a genuine
TOCTOU race in the watchdog/request-lifecycle interaction, now fixed).

**Where this leaves the investigation**: the standard UMDF 1.x USB access path
is provably unused; WinUsb.dll, the flat WDF USB target API, raw
`DeviceIoControl`/`CreateFile`/`ReadFile`/`WriteFile` are all provably unused
(confirmed both never-called across full traced runs and, for `ReadFile`/
`WriteFile`, additionally confirmed importable-but-unused via the DLL's own
import table); the exact worker-thread call chain down to the wait itself is
mapped with high confidence (constructor at RVA `0x180010e38`, dispatcher at
RVA `0x180027fb0`); and real time doesn't help. The strongest remaining
hypothesis is architectural: this driver may depend on kernel-mode-adjacent
WDF machinery (the UMDF host process's own I/O completion port / reflector
driver interaction, or a genuine hardware interrupt notification path) that
a userspace-only bridge fundamentally cannot supply without a much larger
reimplementation effort - i.e. this may be a ceiling on what's achievable via
this project's architecture as it stands, not a specific bug left to find.
Not yet proven either way; the `IWDFUsbTargetDevice`/`Pipe` vtable layouts
documented above remain ready to implement against if a future pass does find
where they're meant to be acquired.

### Correction: `device_obj` vtable slot 42 is NOT `CreateWorkItem` (off-by-one in an earlier pass)

A pass through this investigation initially misread the WDK method-listing
fetch as 0-indexed (matching this project's own 0-indexed `com_stub_N` array)
when it was actually **1-indexed** (`1. QueryInterface`, `2. AddRef`, ...).
This produced a wrong match: `device_obj`'s vtable slot 42 (confirmed live via
a real vtable dispatch through `[vtable+0x150]`, `0x150/8 = 42` exactly) was
identified as `IWDFDevice3::CreateWorkItem` - which looked like an extremely
promising lead (a silently-no-op'd async work-scheduling mechanism would
perfectly explain an eternal wait). **Re-fetched the raw method list and
counted 0-indexed by hand**: slot 42 is actually `AssignS0IdleSettingsEx` (a
power-idle-policy setter - mundane, matches the small/garbage-looking extra
args seen in the live capture, since it only really takes one struct-pointer
argument beyond `self`); `CreateWorkItem` is really at **slot 41**.

Live-checked both the corrected `CreateWorkItem` slot (41) and `CreateInterrupt`
(slot 40, the other plausible async-notification candidate right next to it)
across a full real enroll attempt: **zero hits on either.** Neither is ever
called. This closes off the "work item" and "interrupt object" hypotheses
entirely - this driver doesn't use either mechanism (at least not through this
project's `device_obj` facade) for whatever's supposed to signal capture
completion. Every `device_obj` vtable slot actually observed firing across a
full attempt (4, 6, 15, 16, 17, 19, 25/26/27/34/42) maps to mundane,
already-understood, benign functionality (locking, device-interface
registration, request dispatching, and - 25/26/27/34/42 - power/idle policy
negotiation) - none plausibly connected to the missing completion signal.

**Lesson for future passes**: when consuming a fetched/summarized method
listing to match against this project's own 0-indexed slot numbers, always
re-verify the indexing scheme explicitly (ideally by fetching the raw
unnumbered source and counting by hand, as done here) before trusting a
match - a fetch's own renumbering convention isn't guaranteed to align with
what you need, and a match that "looks compelling" (like the `0x150/8=42`
arithmetic here) can still rest on a wrong table.

### Experiment: manually completing the worker thread's wait - no forward progress

Rather than continuing to hunt for what's *supposed* to signal the
`WaitForSingleObject` (RVA `0x14e41`, blocking on the operation object's
`+0x28` handle - see the live-traced call chain above), tried directly
forcing it: a temporary hook in `tudor_hook_thread_create` spawned a detached
thread that polled `ctx->field_0xf8` (set by the trivial setter at RVA
`0x180027f94` right after the operation object is constructed) until it
appeared, then called this project's own `SetEvent()` on the object's `+0x28`
handle.

(First attempt used GDB's inferior-function-call mechanism, `call (int)
SetEvent(...)`, to test this without a rebuild - that crashed the whole
process with `Fatal glibc error: pthread_mutex_lock.c:426
(__pthread_mutex_lock_full): assertion failed: e != ESRCH || !robust`,
matching the same general class of glibc-assertion problem as the
`pthread_tpp_change_priority` crash noted elsewhere in this doc. This looks
like an artifact of GDB hijacking a thread's execution to make an inferior
call, not something that reflects real (non-debugger) execution - switched to
a real code-level hook instead, which doesn't have this problem.)

**Result: `SetEvent` genuinely succeeded (returned `TRUE`) both times it fired
across a full automated enroll attempt, but nothing observable changed
afterward** - no new `WinUsb_ReadPipe`/`WritePipe` calls, no new
`DeviceIoControl` calls, no additional log output of any kind from the worker
thread. The outer devctrl request still only ever completed via this
project's own 5-second watchdog (`0xc00000b5`), exactly as if nothing had been
signaled at all. No crash, but no progress either.

This rules out the simplest version of "the wait is the only thing missing" -
either the worker thread needs *more* than just this one event (a second
condition, a data field this project never populates that it checks after
waking and silently bails out on, or another synchronization point further
downstream not yet found), or waking up leads it into a path that fails
silently without touching anything this project has instrumented. The
experimental hook was reverted (no diagnostic value beyond this one result,
and any further attempts in this direction would need to also identify and
populate whatever real "capture result" data structure a genuine hardware
completion would have written - not attempted, since the data layout there is
completely unknown).

### Implemented the full IWDFUsbTargetFactory/Device/Interface/Pipe COM bridge - and proved it's unreachable

Given the exhaustive elimination above, went ahead and implemented the
documented UMDF 1.x USB access chain in full, on the chance some as-yet-unfound
precondition (not the interfaces' absence) was what gated the driver from
ever attempting it:

- **`winapi/wdf/usb.c`**: refactored `transfer_req_ctx`/`pipe_transfer_callback`/
  `usb_transfer_start` to work off a raw `(buf, buf_len)` pair instead of a
  `struct wdf_memory*` (needed because this project's own `IWDFMemory` COM
  facades, `com_memory_data`, are a completely different in-memory
  representation than the flat API's `wdf_memory` - handing the flat
  functions one of our COM facades directly would be a real type-confusion
  bug, not just a style mismatch, since COM objects are vtable-first and
  `wdf_memory` isn't). Added `wdf_usb_pipe_format_request_for_{read,write}_raw`
  and `wdf_usb_device_get_num_interfaces` as new raw-buffer/COM-facing entry
  points alongside the existing flat, `wdf_memory`-based ones (both share the
  same underlying `usb_pipe_format_request_for_{read,write}_core` now, no
  duplicated transfer-submission logic).
- **`winapi/wdf/wdf.h`**: exposed `WdfUsbTargetDeviceCreate`,
  `WdfUsbTargetDeviceGetInterface`, `WdfUsbInterfaceGetConfiguredPipe`,
  `wdf_start_request`, and moved `WDF_USB_PIPE_TYPE`/`WDF_USB_PIPE_INFORMATION`
  here from being private to `usb.c` - same "exposed for tudor/com_activate.c"
  pattern already used for `WdfDeviceCreate`/`WdfIoQueueCreate`/etc.
- **`tudor/com_activate.c`**: added COM facades for
  `IWDFUsbTargetFactory`/`IWDFUsbTargetDevice`/`IWDFUsbInterface`/
  `IWDFUsbTargetPipe`, wired `QueryInterface(IID_IWDFUsbTargetFactory)` on
  `device_obj` to return a real factory facade instead of the generic
  unimplemented-alias fallback, and added `IWDFIoRequest::Send` (slot 23) so
  a `FormatRequestForRead`'d request can actually be dispatched. Real vtable
  slot numbers for all of these were verified by fetching `wudfusb.h`/
  `wudfddi.h` raw and reading the flattened C-style vtable struct directly
  (not a summarized/renumbered AI fetch - see the slot-42 postmortem above for
  why that distinction matters; this time every number was cross-checked
  against the unambiguous `IWDFXxxVtbl` struct definition itself).

Builds clean, and automated regression testing confirms no crash or behavior
change to the existing (already-working) graceful-failure path.

**Then checked, statically, whether this could ever possibly matter: searched
both driver DLLs' raw bytes for the IID constants
(`IID_IWDFUsbTargetFactory`/`Device`/`Interface`/`Pipe`/`Pipe2`) this new
bridge exists to serve. None of the five appear anywhere in either binary.**
This is stronger than "never observed calling it live" (which was already
known) - it's proof the driver's own compiled code contains no possible
reference to these interfaces at all, under any input or code path. The
entire UMDF 1.x standard USB-target mechanism is provably, structurally dead
for this specific driver build - not gated behind an unmet precondition
somewhere else, genuinely absent. The new bridge code is real, tested, and
harmless to keep (it's simply unreachable), but this closes off the
"documented mechanism, just need the right trigger" hypothesis completely.

**This means the real mechanism this driver uses to reach its sensor hardware
is still unidentified, and is not any of**: `WinUsb.dll`, the flat WDF USB
target API, the COM UMDF 1.x USB target API, raw `DeviceIoControl`/
`CreateFile`/`ReadFile`/`WriteFile` (all confirmed unused across full traced
runs), `CreateWorkItem`/`CreateInterrupt` (confirmed unused). Every standard
Windows/WDF driver-to-hardware channel this investigation could think of and
check is now eliminated. What remains untried: a wholesale disassembly sweep
of the worker-thread's full instruction range (the live trace so far only
followed one path several calls deep, not the entire reachable graph) looking
for *any* remaining unresolved import call or vendor-specific mechanism (a
companion service process, named pipe/shared memory IPC, or similar) that
wouldn't show up as a standard Win32 API call at all - none of which this
project's current instrumentation is set up to detect.

### Continued: connecting back to the pre-session `0x1800125dc` "purpose dispatch" investigation

Went back to first principles: instead of guessing at exotic IPC mechanisms,
did a genuine breadth-first live instruction walk of the worker thread's own
code, starting from RVA `0x1a9f0` (its first real callee, per the earlier
live-confirmed chain). Key methodology fix needed to make this tractable: a
plain `nexti`-stepping loop that steps *over* every call gets permanently
stuck the moment it steps over a call that itself blocks (exactly what
happened during an earlier attempt) - fixed by wrapping each step in a
watchdog thread that interrupts (`SIGINT`) after a few seconds if the step
hasn't returned, revealing *which specific call* is the blocking one instead
of just hanging.

This walk got substantially further than any previous live trace (~300
instructions into `0x1a9f0`, past the already-known constructor call at RVA
`0x10e38`) and found the block inside a call to **RVA `0x115c0`**, which
itself calls **RVA `0x1800125dc`** - and that address is not new: it's the
*exact* function a pre-session pass through this investigation already
identified (see "The `0x80070070` chain" above) as a large function that
"dispatches on a 'purpose' value at `ctx+0x80` into timeout-configured
polling loops (confirmed via a real `GetTickCount`-based elapsed-time check -
a genuine timed poll, not queue logic)", gated behind the exact same
`[obj+0x20] != 0` check already fixed by patching `ctx->field_0x100`. This
session's live worker-thread trace and that much older static-disassembly
finding are the *same code path*, now connected end-to-end for the first
time: thread entry (`0x1c21c`) → `0x1a9f0` → `0x115c0` → `0x1800125dc`
(purpose dispatch + poll) → (eventually) the already-known
`WaitForSingleObject` at RVA `0x14e41`.

**Live-confirmed `ctx+0x80` (the dispatch value) is `2` for a real enroll
attempt** (not `4`, `WINBIO_PURPOSE_ENROLL`'s public value - this is a
different, internal-only operation-type enumeration, not the WINBIO_PURPOSE
value directly). Value `2`'s handler branch is exactly where `objdump`'s
linear disassembly breaks down into garbage bytes right after a
`lea rax,[rip+...]` - almost certainly a compiler-generated jump table for a
nested switch, which is why static analysis alone couldn't fully resolve this
branch; live tracing was needed instead.

### The wait is a genuine retry/poll loop, and the driver's own `SetEvent` on it is the already-known "abandon" signal, not completion

Combined a live `WaitForSingleObject`/`SetEvent` breakpoint trace with the
earlier force-signal code hook (this time also polling `ctx->field_0x198` -
the CreateThread-returned worker thread handle, per RVA `0x27fb0` - for a
full 5 seconds after signaling, to see whether the worker thread actually
finishes). Findings, all confirmed live in a single combined run:

- **The worker thread waits on the *same* event handle more than once** -
  once before this project's manual `SetEvent`, and again afterward. This
  confirms the "wait" is not a one-shot completion wait at all: it's an
  auto-reset event inside a loop (matching the pre-session finding's own
  "timeout-configured polling loop" description precisely) - the worker
  re-arms and waits again after each iteration rather than proceeding once
  woken.
- **The real driver *does* eventually call `SetEvent` on this same handle
  itself** - from the main thread, later, after this project's own manual
  signal. This is not a newly-discovered success signal, though: it lines up
  exactly with the already-documented finding (see "Experiment: manually
  completing the worker thread's wait" above and the original 0x8009800B
  investigation) that the only two `SetEvent` calls observed anywhere in a
  full run both fire as part of the *next* devctrl's own cleanup/abandon path
  releasing the *previous* attempt's context - not a "capture succeeded"
  signal. Confirms, with much more precision than before, that this event is
  never signaled with success semantics by anything in the reachable code.
- **`ctx->field_0x198` (the worker thread's own handle) never clears, for the
  full 5-second observation window after manually signaling the event** -
  meaning even after being unblocked (repeatedly), the worker thread does not
  proceed to actually terminate within that window. Combined with the
  poll-loop finding above, the most likely explanation: the loop's exit
  condition isn't just "event signaled" but a combination of "event signaled
  *and* some data field (plausibly something under `ctx->field_0x100`, this
  project's quine-patched object, or `[obj+0x20]`/`+0x34`-style result slots
  seen throughout `0x1800125dc`'s disassembly) indicating the result is
  genuinely ready" - so blindly signaling the event without also making that
  data check pass just causes another loop iteration, not real progress. This
  exact data layout/condition has not been identified.

**Also separately found (same investigation session) a second, distinct
`WaitForSingleObject` call site, RVA `0x28782`**, which waits with an
`INFINITE` timeout specifically on the worker thread's own `HANDLE`
(`ctx->field_0x198`, confirmed by direct comparison in the disassembly right
after the wait returns) - i.e. this is a `WaitForSingleObject`-based
"thread join": something (running on a different, non-worker thread) blocks
until the worker thread fully terminates. Since the worker thread itself
never terminates (per the point above), this outer wait also never resolves,
compounding the block. Not yet determined which caller this join belongs to
or what runs after it resolves.

**Next concrete step, not yet done**: identify the actual data field/value
`0x1800125dc`'s poll loop checks after each wake (live-breakpoint the exact
compare instruction inside the loop, not just the wait call, to read what
value it wants and what our quine-patched object actually provides there) -
this is now a much more specific, tractable target than the earlier
"somewhere in this huge function" state, and could be the final piece needed
for the worker thread to genuinely complete rather than just loop.

### Found and confirmed the real success path - and why it can never succeed with this project's fake data

Followed through on the above: disassembled the code immediately after the
`WaitForSingleObject` at RVA `0x14e41` returns, and mapped the *entire* real
success-path chain for the first time (all four steps live-confirmed reached,
not just statically inferred, per the trace below):

1. RVA `0x14e4f`: checks the wait's own return value (`[rsp+0x6c]`, the `eax`
   `WaitForSingleObject` returned) - if `0` (`WAIT_OBJECT_0`, i.e. genuinely
   signaled, not timed out/failed) and a cancel flag at the operation object's
   `+0x3c` is also `0`, falls through to the success path at RVA `0x14e95`
   instead of the (already-known) error path.
2. RVA `0x14e95`-`0x14ec6`: a second flag check (`+0x44`, also `0` by
   default), then reads the operation object's `+0x30` field as an overall
   status code - if `0` (success), continues to RVA `0x14fb6`.
3. RVA `0x14fb6`-`0x14fe2`: calls **RVA `0x180046000`**, passing
   **`ctx->field_0x100` directly as the "self" argument** (the exact object
   this project constructs via the real driver factory and quine-patches -
   see `try_construct_real_field0x100_obj` in `driver.c`), plus a hardcoded
   input value of `1` and an out-parameter buffer.
4. `0x180046000` does its own WPP-tracing preamble, then dispatches on that
   hardcoded `1` input (maps to internal case `2`), and calls **RVA
   `0x180061000`**, passing `*(ctx->field_0x100->field_0x8)` - i.e.
   dereferencing this project's own quine-patched pointer *again* - as the
   "self" argument, along with the purpose value (confirmed live as `2`) and
   another out-buffer.
5. `0x180061000` does more WPP tracing, then computes an output byte based on
   reading a *locally zero-initialized* buffer (not anything read from our
   quine object directly at this specific point) and the purpose value,
   writing a result into the caller's out-parameter - the computed value
   ultimately propagates back up as the operation's overall result status.

**Live-confirmed via a combined trace** (the code-level `SetEvent` hook from
the earlier experiment, run together with GDB breakpoints on RVA
`0x14e95`/`0x14ec6`/`0x46000`/`0x61000`): once the wait is manually signaled,
execution genuinely reaches all four steps, multiple times, with `rcx` at
`0x180061000` matching the expected quine-dereferenced pointer and `rdx=2`
matching the confirmed purpose value - no crash, this is real, live-executing
driver code processing this project's fake object exactly as this static
trace predicted.

**And also live-reconfirmed (from the round-2 experiment, restated here for
context): after this chain runs, the worker's `WaitForSingleObject` gets hit
again on the very same handle** - meaning the loop doesn't exit after this
"success" branch; it re-arms and waits again. Combined with the chain above,
this now has a precise explanation: the fields feeding this chain (the
operation object's own `+0x3c`/`+0x44`/`+0x30`, and whatever
`0x180046000`/`0x180061000` compute from them and from `ctx->field_0x100`'s
data) are all derived from **this project's own fake/quine-patched
object**, not real captured hardware state - so whatever overall status this
chain computes is very unlikely to ever equal "genuinely complete," and the
outer poll loop just keeps retrying until its own overall elapsed-time budget
runs out, never actually finishing.

**This sharpens the core remaining mystery to something much more specific
than "an unidentified mechanism somewhere"**: real hardware communication, if
it exists anywhere in this driver's reachable code, must populate real data
reachable from `ctx->field_0x100` (specifically, real data behind
`field_0x100`'s own `+0x8` sub-object, since that's what `0x180061000`
consumes) - and this project's own code is what currently substitutes a fake
quine object there in the first place, precisely because the real
constructor at RVA `0xf6e00` leaves that particular sub-object unpopulated
and a separate real-driver consumer crashes on a NULL/garbage value there
without the quine substitution (see `try_construct_real_field0x100_obj`'s own
comments). This means the missing mechanism is almost certainly upstream of
this entire chain: something that's supposed to populate a *real* object at
`ctx->field_0x100 + 0x8` (or update it after `field_0x100`'s own construction)
with genuine hardware-derived state, before the worker thread ever reaches
this wait. Not yet found - the natural next step is tracing backward from
`try_construct_real_field0x100_obj`'s call site to find what (if anything)
this project currently prevents from running by substituting a fake pointer
there, rather than continuing to trace forward through the consumer chain.

### Found the real construction path for `ctx->field_0x100` - and two more real, still-unpopulated fields it depends on

Acted on the "trace backward" step above: searched the entire DLL for direct
callers of `FIELD0X100_CTOR_RVA` (`0xf6e00`, the raw constructor this project
was calling directly) and found exactly **one real caller anywhere in the
binary, at RVA `0xa0fc0`**. This is a generic type-tagged constructor
dispatcher: `(uint32_t type_tag /*ecx*/, void *self /*rdx*/) -> uint32_t
status`, which validates `type_tag == 0xCA` (matching the exact tag byte
`0xf6e00` itself writes to the object's offset 0 - confirming this dispatch
case genuinely is the intended construction path for this object type, not a
coincidence) before calling `0xf6e00`. Beyond that, it does real work this
project's direct call was skipping entirely: it allocates a separate real
`0xD0`-byte sub-object (via the project's already-known allocator,
`call 0x1800d88c0`), runs a real constructor on it (RVA `0xfb9c0`), and
stores the result at the outer object's **offset `+0x248`**.

This is now wired up in `try_construct_real_field0x100_obj()` in `driver.c`
(calls the dispatcher instead of the bare constructor) and **live-tested
successfully: no crash, no regression, and `field_0x248` now holds a genuine
non-null heap pointer** instead of the NULL it always had before. `field_0x8`
is left untouched by both the raw constructor and this dispatcher, so the
quine-pointer fallback built for it earlier is still applied afterward as a
safety net (a separate consumer dereferences it directly, unrelated to any
vtable).

**What `0xfb9c0` (the `+0x248` sub-object's own constructor) does**: populates
14 function-pointer slots on the new object (offsets `+0x38` through `+0xa8`,
each via `lea`+`mov`, a real vtable-population pattern matching how the OUTER
object's own embedded vtable at `+0x70`..`+0x110` is populated by `0xf6e00`),
then calls through the just-installed `+0x38` slot once before returning
success. That `+0x38` slot (RVA `0x1800fc450`) turned out to be a trivial
`xor eax,eax; ret` stub - so this "invoke after construct" call is a no-op in
this build; the sub-object's real work happens through its *other* slots,
called later by other consumers. One of those other slots, `+0xa8` (RVA
`0x1800fc470`), is *not* a stub: it calls a fixed helper (`0x180117100`) and
writes three populated fields back onto a caller-supplied object (`+0xc0`,
`+0xc4`, `+0xc8`) - real, non-trivial logic, confirming this sub-object is
functionally alive, not a dummy.

**Traced one of the outer object's own embedded-vtable slots and found a
second real, still-NULL field**: `ctx->field_0x100`'s embedded vtable slot
`+0xa0` (populated by `0xf6e00` to point at RVA `0x1800f8c40`) is called from
inside a large "prepare capture data" routine (~RVA `0xa01fd`-`0xa08cf`) that
allocates/frees up to 77 array entries and also calls two more real methods
*through the `+0x248` sub-object* (`0x1800fa7d0`, `0x1800fb960`) - direct,
concrete evidence that real driver code elsewhere already depends on
`+0x248` being a genuine object, exactly as this session's fix now provides.
Inside `0x1800f8c40` itself: it reads `self->field_0x240` (a *separate*
sibling field to `+0x248`, also explicitly zeroed by the `0xf6e00`
constructor - confirmed via direct disassembly, `mov QWORD PTR
[rax+0x240],0x0`) and branches on whether it and its own `+0x28` sub-field
are non-null. **This project's dispatcher fix does not populate `+0x240` -
it remains NULL**, and no other real code path writing a genuine pointer into
`ctx->field_0x100 + 0x240` specifically has been found (a handful of
`+0x240`/`+0x248` hits elsewhere in the binary were checked and are
offset collisions on unrelated, differently-shaped structs - e.g. RVA
`0xa87cb` uses its own object's `+0x248` as a plain element *count* and
`+0x240` as a count-sized allocated array, nothing to do with this object).

**Traced the `+0x8` consumer chain (from the prior section) all the way down
to its real endpoint, and it clarifies rather than changes the core
conclusion**: `0x180061000` (step 5 above) dereferences
`ctx->field_0x100->field_0x8` three times in total and ends up calling
through a function pointer at offset `+0x10` of the final dereferenced
object, passing a *sibling* field at `+0x18` of that same object as the
call's own first argument - i.e. a bound-callback pattern (`{ ..., func_ptr
@ +0x10, context @ +0x18, ... }`), not a plain C++ vtable. The call itself
goes through two more fixed real functions first (`0x1800ea9d0` then
`0x1800f12d0`, both genuine, non-stub code with WPP-tracing preambles and
real argument marshalling) before reaching that final indirect call, with a
hardcoded property-key-shaped constant (`0x938ae038`) as one of the
arguments; a sibling constant `0x938ae010` is checked separately nearby,
suggesting a small family of related property keys. The single byte this
callback is expected to write back is then read directly by
`0x180061000` and remapped 0→0/1→1/2→2/3→3(+log) into `ctx->field_0x100`'s
own overall status output - i.e. **this whole chain exists purely to fetch
one status byte from whatever real object should live behind `field_0x8`,**
confirming (not merely re-asserting) the prior section's conclusion.

**Net effect of this pass**: found and fixed one genuinely missing piece of
real construction (`+0x248`, done, verified safe), and found that the
remaining blocker is **not one field but at least two** (`+0x8` and `+0x240`)
on the same object, both explicitly zeroed by the real constructor, both
consumed only by code miles deep in this driver's own capture-processing and
property-retrieval logic - never by anything resembling a USB/WDF/Win32 call
this project could intercept. This is consistent with, and sharpens, the
standing conclusion from the exhaustive WinUsb/WDF/import elimination further
up this document: **these fields are architecturally meant to be filled in
as a side effect of a real USB transaction completing** (property values and
capture-buffer state read back from real hardware), and since no such
transaction is ever issued anywhere in this reachable code (proven via the
`DBGIMPORT`/`DBGWDF` abort mechanism - still zero hits across every test this
session), nothing populates them, by design of the real driver, not by a bug
in this project's bridging code. The open question going forward is not "what
already-connected code should write these fields" but "what has this project
not yet connected at all" - most likely a callback registration, IOCTL, or a
COM interface neither this DLL's own imports nor `synaBscAdapter52.dll`'s
side has been checked for yet (`synaBscAdapter52.dll` has not been searched
this session - all tracing above is confined to `synaWudfBioUsb52.dll`).

### Live trace correction: the `0x938ae0XX` property-get chain is actually reached through `g_fake_ctx_obj` (`field_0x1b8`), not `field_0x100`'s `field_0x8`

Ran a real (fingerless) live enroll attempt with the dispatcher fix in place,
with temporary `log_debug` instrumentation added to `WaitForSingleObject`/
`SetEvent` in `winapi/sync.c` (reverted after this test - see below). Two
things this revealed, one a correction to the static trace above, one a
reconfirmation of the standing conclusion:

1. **The `0x938ae0XX`-family property-get calls are real and do fire on every
   single capture attempt** - but they land on **`g_fake_ctx_obj`'s vtable
   slot #2** (the fake object this project already built for
   `ctx->field_0x1b8`, the StartCapture worker thread's own context object -
   see `try_construct_real_field0x100_obj`'s neighbor in `driver.c`), not on
   `ctx->field_0x100->field_0x8` as the static trace assumed. Live-observed
   keys: `0x938ae058` (size `0x10`), `0x938ae060` (size `0x14`), `0x938ae064`
   (size `0x4`) - a different specific member of the same `0x938ae0XX` family
   the static trace found (`0x938ae038`/`0x938ae010`/`0x938ae048`), confirming
   both are hitting the same real property-key namespace, just from different
   starting objects/call sites. Static, symbol-less whole-DLL tracing can't
   always tell which of several structurally-identical call sites actually
   fires at runtime - this is a concrete example, corrected by observation.
2. Every slot on `g_fake_ctx_obj` (including #2) was a pure logging stub that
   `return`s `0` without ever touching the caller-supplied output buffer -
   i.e. whatever property value the real driver asked for came back as
   **success status with an untouched (garbage) buffer**, not real data.
   Changed slot #2 specifically to `memset` the buffer to zero before
   returning (a deterministic, defensible default - not a guess at real
   semantics, which remain unknown - see `fake_ctx_obj_stub_2` in
   `driver.c`). Compiles clean, live-tested: **no regression, but no change
   to the observable outcome either** - the worker's `WaitForSingleObject`
   still doesn't resolve naturally; this project's own 5s watchdog still
   fires first with `0xc00000b5`, in exactly the same position in the log as
   every prior run. This is a real (if negative) result: whatever this
   property-get call is *for*, it's not itself gating the wait's completion -
   it's queried once during setup, before the wait is even entered.
3. **Reconfirms, live, with today's fix in place, the existing "abandon
   signal" finding from the round-2 experiment earlier in this document**:
   `SetEvent` genuinely is called on the worker's wait handle in a normal
   run - but only *after* this project's watchdog has already forced
   completion with `0xc00000b5` and the CLI has issued the `0x44201c`
   discard IOCTL; the driver's own teardown code for the abandoned capture is
   what calls `SetEvent`, not a genuine completion path. Confirms the core
   blocker is unchanged: nothing signals genuine completion within the
   watchdog's window, no matter what this session's fixes touch.

Net: two more small, real, verified-safe improvements landed
(`try_construct_real_field0x100_obj`'s dispatcher call, `fake_ctx_obj_stub_2`'s
buffer zeroing), plus a corrected map of which fake object actually
intercepts the property-get chain live - but the observable symptom
(watchdog-forced `0xc00000b5` discard) is unchanged. The `WaitForSingleObject`/
`SetEvent` diagnostic logging added to `winapi/sync.c` for this test was
temporary and has been reverted; if picking this up again, re-add it
(`log_debug` at entry/exit of both functions) rather than re-deriving the
call sites from scratch.

### Architectural clue from the driver's own INF: `UmdfDispatcher=WinUsb`, and a real (but dead) missing link

Checked the driver's own INF (`synaWudfBioUsbPQIDongleProd.inf`, extracted
into the build tree at
`build/libtudor/synaBscAdapter52.dll.p/windrv/`) for the first time this
session - and also confirmed, via `git log` on this fork, that this whole
project is a fork of the general-purpose upstream `synaTudor` project
(supports other, already-*working* Synaptics Tudor sensors via a **different**
driver generation, `synaFpAdapter104.dll`/`synaWudfBioUsb104.dll`, sourced
from a Lenovo installer). This fork's only real change (`bda736a`, 18 lines)
was repointing the driver download at a **different Microsoft Update
Catalog `.cab` package** (`synaBscAdapter52.dll`/`synaWudfBioUsb52.dll`, the
"52" generation, specifically for this hardware ID) - i.e. this project's
extensively-proven-working bridge was designed against generation "104", and
generation "52" (what this session has been reverse-engineering) is a
different enough internal architecture that essentially none of that
experience transfers directly. Explains why this driver needed this
session's whole quine/dispatcher/layout-mismatch treatment where other
Tudor sensors reportedly just work.

The INF's driver section for this device (`Whitebox_Install`/`Biometric_Install`)
declares:
```
UmdfDispatcher=WinUsb
UmdfLibraryVersion=1.11.0
```
`UmdfDispatcher=WinUsb` is a real, meaningful UMDF 1.x INF directive: it
configures the kernel-mode reflector (`WUDFRd.sys`) to service low-level USB
transfers automatically for requests sent through the device's **default I/O
target**, so the driver DLL is expected to talk to hardware via
`IWDFDevice::GetDefaultIoTarget()` + `IWDFIoTarget::FormatRequestForIoctl`/
`Send` (WinUsb-style `IOCTL_WINUSB_*` codes), *not* via the explicit
`IWDFUsbTargetDevice`/`Pipe` COM object model this session already implemented
and proved unreachable (binary GUID search - see above). This is a second,
independent confirmation of that conclusion: a UMDF 1.x driver picks *one* of
these two USB access models via its INF, never both, and this INF picks the
`GetDefaultIoTarget` + raw-IOCTL model.

Checked `com_activate.c`'s existing `com_get_default_io_target`
(`IWDFDevice` slot 11, `libtudor/src/tudor/com_activate.c:408`): it exists,
but per its own comment, returns a **generic-stub-backed facade** - every
method on the returned `IWDFIoTarget` (including `Send`/
`FormatRequestForIoctl`) just logs and returns `S_OK` without decoding the
IOCTL or touching real hardware. This project already has real, working USB
transfer code sitting completely unused for this purpose - `winapi/winusb.c`
(`libusb_control_transfer`/`libusb_bulk_transfer`) and `winapi/wdf/usb.c`
(`libusb_submit_transfer`) - neither wired to this facade.

**However: live-confirmed (grepping the full `-v` trace from the previous
enroll attempt, 570k+ lines) that `com_get_default_io_target` is never
actually called** - only `com_create_io_queue` (the devctrl queue) fires.
So this is a real, concrete, *documented* architectural gap - the natural
place real capture I/O should eventually flow once the driver actually
reaches it - but it does not by itself explain the current blocker, since
the driver's own code never gets far enough to call it. This sharpens the
open question one more notch: the real remaining mystery is still *why the
StartCapture worker thread's own internal purpose-dispatch/poll logic never
reaches the point where it would ask for the default I/O target at all* -
not what happens after it does. Fixing `com_get_default_io_target` to do real
I/O is worthwhile future work (a real, permanent gap, independent of
anything else), but won't by itself unblock anything until whatever gates
reaching it is found.

### Ruled out: every currently-"unimplemented" `device_obj` vtable slot hit live is a benign power/diagnostic method, not the blocker

Followed up on the live trace's `[WRN] Unimplemented IWDFDeviceInitialize-ish
COM vtable slot #N called!` lines for `device_obj` (`self=0x5555555e87a0` in
that run - a fixed identity confirmed via `com_create_io_queue`'s own log
line using the same address). Slots seen live: 15, 16, 17, 26, 27, 34, 42
(plus 4/6 on unrelated, stack-allocated transient objects - not `device_obj`,
not investigated further).

Fetched the actual UMDF **1.11** `wudfddi.h` (`IWDFDeviceVtbl`/
`IWDFDevice2Vtbl`/`IWDFDevice3Vtbl`, raw flattened C structs, not an
AI-summarized doc page - see the methodology note earlier in this document
about why that distinction matters) and version-matched it against this
INF's own `UmdfLibraryVersion=1.11.0` line, then 0-indexed exactly as done
for the previously-verified slots (`GetDefaultIoTarget`=11,
`CreateIoQueue`=14, `ConfigureRequestDispatching`=19, `MapIoSpace`=35,
`CreateInterrupt`=40, all cross-checked as still correct by this recount -
good confidence in the counting method). Result, every live-hit slot
identified:

| Slot | Method | Category |
|---|---|---|
| 15 | `CreateDeviceInterface` | PnP interface registration (already known safe to no-op per the existing comment above `com_generic_stub_impl` - the driver only treats this as fatal via a differently-shaped failure) |
| 16 | `AssignDeviceInterfaceState` | same as above |
| 17 | `RetrieveDeviceName` | diagnostic/naming only |
| 26 | `StopIdle` (called live with `WaitForD0=TRUE`) | S0 idle power management |
| 27 | `ResumeIdle` | S0 idle power management |
| 34 | `GetSystemPowerAction` | power management |
| 42 | `AssignS0IdleSettingsEx` | power management |

**None of these are capture/USB I/O methods** - they're PnP/power-management
plumbing this Linux bridge has no real equivalent for and is correct to
no-op (there's no real ACPI power-state machine underneath, so "always
already at full power, S_OK" is the right answer, not a gap). This closes
off "an unimplemented `device_obj` method is silently eating the real
completion signal" as a hypothesis with actual evidence, not just absence of
a crash. Combined with the `GetDefaultIoTarget` finding above, the
remaining mystery is unchanged and now has no more low-hanging COM-vtable
candidates left on `device_obj` itself: **something inside the StartCapture
worker thread's own purpose-dispatch/poll logic (the `field_0x100`/`+0x8`/
`+0x240` chain from earlier in this document) never progresses far enough to
even attempt asking for real I/O in the first place.** `synaBscAdapter52.dll`
(the adapter half of this bridge) remains completely unexamined this
session - all tracing across this entire document is confined to
`synaWudfBioUsb52.dll` - and is the most promising unexplored surface for
the next pass.

### Correction: the worker's wait is purely event-driven (`INFINITE` timeout), not a timed poll - sharpens where to look next

The live trace (`WaitForSingleObject(0x..., 4294967295) entered`) confirms
the worker thread's wait uses `INFINITE` as its timeout, not a bounded
one - so the earlier "genuine retry/poll loop" characterization is imprecise:
there's no self-timeout at this level at all. The *only* way this
specific wait can ever resolve is a real `SetEvent` on that exact handle;
this project's own 5s watchdog (a wrapper around the whole devctrl request,
not this wait) is what currently forces things along, and the driver's own
`SetEvent` (confirmed live, see above) only fires as post-discard teardown,
never as genuine completion.

Given that, and given `com_get_default_io_target` is confirmed **never**
called across a process's entire lifetime (not just during capture - checked
the full trace from process start), the real USB communication machinery
this driver needs (whatever specific shape it takes) is never initialized at
*any* point, not just never used during capture. That points one level
earlier than everything traced so far in this document: at **device
start/`D0Entry` time**, not capture-dispatch time. This session's tracing
has been almost entirely focused on the StartCapture dispatch path forward;
tracing forward from device-add/start instead (where a real driver would
typically configure a continuous USB reader or open its default I/O target
once, up front) is the natural next thread - not yet attempted this session.

**Closed a real gap in the earlier "no unresolved import/WDF function is ever
called" proof while investigating this**: that proof (via the
`DBGIMPORT`/`DBGWDF` abort mechanism) only covers *statically*-linked PE
imports resolved at load time - it does not cover functions obtained
dynamically via `LoadLibraryA`+`GetProcAddress` at runtime, which resolve
through a completely different code path (`resolve_windows_api`) that never
aborts on success. The live trace shows the driver *does* call
`LoadLibraryA("WINUSB.DLL")` then `GetProcAddress` for `WinUsb_Initialize`/
`ReadPipe`/`WritePipe`/`ControlTransfer`/etc., all of which resolve
successfully to this project's real, libusb-backed implementations in
`winapi/winusb.c` - a plausible way real capture I/O could show up that the
static-import proof wouldn't have caught. Checked directly: `winusb.c`'s real
implementations (`WinUsb_ReadPipe` etc.) all have their own `log_debug` calls
on actual invocation (not just resolution) - grepped the full trace and
**confirmed zero real invocations, only the `GetProcAddress` resolution
lines**. So this specific gap is now closed with actual evidence rather than
an untested assumption: the driver resolves these functions but never calls
them either, consistent with everything else found this session.

## Enrollment hang (devctrl `0x44200c`)

`tudor_enroll_start` (`device.c`) calls, in order: `ClearContext` x3,
`CreateEnrollment`, `SetEnrollmentParameters` on the WINBIO pipeline adapters.
Live testing shows the CLI's `e` command hangs indefinitely right after the
"Enter finger index" prompt - the process never reaches "Put your finger on the
sensor", and never crashes; it just stops responding.

Confirmed via a live all-thread backtrace (see "Getting a backtrace on a hung
process" in Methodology notes below) that this is a genuine **deadlock**, not a
spin or an infinite loop:

- The **main thread** is blocked inside a real Win32 `WaitForSingleObject` call,
  reached synchronously from `winwdf_devctrl_file` while dispatching devctrl code
  `0x44200c` (a new, enrollment-only code never seen during identify - not
  `0x440008`/`0x440010`/`0x440014`). The call chain is entirely inside the driver's
  own real code; the main thread hasn't even returned from the devctrl dispatch
  yet, so this isn't caused by this project's own devctrl watchdog (which only
  arms *after* `winwdf_devctrl_file` returns).
- A **second thread** is simultaneously blocked inside `EnterCriticalSection`,
  called from a small, generic "compare ptr for NULL, then either log-and-return
  `0x70`(112) or `EnterCriticalSection(ptr)` and return 0" helper in the driver
  (RVA `0x1800d95d0`) - i.e. it's trying to acquire a lock that's already held,
  presumably by the main thread (or by whatever the main thread is itself waiting
  on to release it) - a classic AB-BA-style deadlock.

Two things this hang is confirmed **not** caused by, ruled out live this session:

- **Not the `ctx+0x1b8` real-vs-fake object choice** - forcing the fake object
  (temporarily reverting `try_construct_real_ctx_obj` to always fail) reproduces
  the identical hang at the identical devctrl code, so this isn't a side effect of
  that fix.
- **Not a second, unpatched worker thread** - added temporary diagnostic logging
  to `tudor_hook_thread_create` for *every* `CreateThread` call regardless of RVA
  (not just `STARTCAPTURE_THREAD_RVA`). Only the warm-up capture's own
  StartCapture thread ever spawns; no new thread is created for the `0x44200c`
  devctrl. So this isn't the same class of bug as the StartCapture worker
  thread's context object gap - there's no second thread here needing a similar
  patch.

Given no new thread spawns, the second (blocked-on-lock) thread is most likely a
**leftover thread from the warm-up capture's own StartCapture cycle** (during
`tudor_open`, before enrollment ever starts) that either never fully exited or
left a lock held past its own completion.

**Root-caused and fixed.** The blocked second thread (confirmed via `info proc
mappings` + a live backtrace: frame `EnterCriticalSection` → return address RVA
`0x1800d9612`, i.e. mid-way through the driver's own "safe enter" helper at RVA
`0x1800d95d0`) is genuinely calling `EnterCriticalSection` with `rcx` equal to
**`&g_fake_quine_obj`** (this project's own quine object used for
`ctx->field_0x100`, confirmed by comparing the live pointer against `nm`'s
symbol table + the mapping's file-offset base). The driver was never patched to
expect this - our quine object was never a real `CRITICAL_SECTION`, so its
`LockSemaphore` field (offset `0x18`) was just another self-pointer, and
`pthread_mutex_lock()` on a non-mutex pointer is undefined behavior that
manifests as a permanent hang.

Fixing this exposed **two further SIGSEGVs**, each following the same shape: the
quine object's offset `0x8` ("slot1") turned out to be read for *multiple,
incompatible* purposes by different driver code paths, not just the
`LockCount`/`RecursionCount` pair:

1. A separate consumer reads `obj->field_0x8` as a raw pointer and
   double-dereferences it (chain ending in a `cmp eax,6`-style dispatch) -
   confirmed live by setting slot1 to `NULL` and observing `RAX=0` at the fault.
2. The **real** structural pattern (found by tracing a live hardware
   watchpoint on slot1's memory) is triple indirection: `obj.slot0`
   ("vtable"-like) points at an object whose *own* `field_0x8` gets an
   `InterlockedIncrement`-style bump (a genuine COM-style `AddRef`), while
   `obj.slot1` separately points at the actual lock object, whose `slot0`
   dereferences back to itself and is what's passed to
   `Enter/LeaveCriticalSection`. A plain self-referential quine collapses
   "refcount target" and "the pointer chased for locking" onto the *same*
   memory, so the `AddRef` write corrupts the very pointer being dereferenced
   moments later (confirmed live: the corrupted value is always the original
   self-pointer with its low byte incremented by exactly 1, landing on a
   non-canonical address on dereference). This pattern recurses: it fires once
   on `ctx->field_0x100`'s own object, then *again*, directly, on the lock
   object that object resolved to.

**Fix** (`driver.c`'s `get_fake_quine_obj`, `winapi/sync.c`):
- `EnterCriticalSection`/`LeaveCriticalSection` no longer touch
  `LockCount`/`RecursionCount`/`OwningThread` at all - nothing in this codebase
  ever reads them back, the real (recursive) pthread mutex already tracks its
  own recursion internally, and writing to them was what let the corruption
  cascade in the first place.
- `ctx->field_0x100` now resolves through **four** dedicated, purpose-separated
  quine blocks instead of one self-referential object: the outer object's
  `slot0` (refcount target) and `slot1` (lock target) point at two different
  objects; the lock object (`g_fake_quine_obj_cs`) itself gets fed through the
  *same* AddRef+lock pattern a second time, so it in turn needs its own
  `slot0` redirected to the (shared) refcount sink and its `slot1` routed
  through a small dedicated "self-pointer" object so it still resolves back to
  itself for the second `Enter/LeaveCriticalSection` call. `g_fake_quine_obj_cs`'s
  `slot3` (offset `0x18`, `LockSemaphore`) holds a real, `PTHREAD_MUTEX_RECURSIVE`
  mutex.

**Confirmed live**: enrollment (`e` → finger index) now runs cleanly through
`patch_device_context`, both `Enter`/`LeaveCriticalSection` cycles, back to
"Loading device records..." and the interactive menu, accepts the enroll
command and finger index, and reaches the point of waiting for an actual
physical finger touch on the sensor - no crash, no hang. This is as far as
automated CLI-driven testing can go; a physical finger touch is needed to
confirm the capture itself succeeds and enrollment completes.

If a *third* recursion of this AddRef+lock pattern turns up on some new object
(same symptom: a byte-shifted self-pointer, findable via the same
hardware-watchpoint technique — see Methodology notes), apply the same fix
shape: shared refcount sink for `slot0`, dedicated self-pointer object for
`slot1`.

### Traced `synaBscAdapter52.dll`'s own `StartCapture` end-to-end - confirms it's a correctly-delegating thin wrapper, not a hidden second bug

`synaBscAdapter52.dll` (the "Adapter" DLL) had never been examined this
session - all tracing above is `synaWudfBioUsb52.dll` only. Its role: it
exports `WbioQuerySensorInterface`/`WbioQueryEngineInterface`
(`tudor_init` in `driver.c` calls these), and `device.c`'s
`tudor_enroll_capture` calls `tudor_sensor_adapter->StartCapture` directly -
a **real exported function inside this DLL**, not something this project
implements. Since `WINBIO_SENSOR_INTERFACE` is a documented, stable
Microsoft struct (already correctly typed in `winbio.h`), and this DLL is
~360KB (vs. 2.6MB for the sensor driver) with plain, unobfuscated,
easy-to-follow code, this was a much more tractable target than more of the
sensor driver's own internals - found its live RVA (add a temporary
`log_debug` of `tudor_sensor_adapter->StartCapture` right after
`WbioQuerySensorInterface` succeeds in `driver.c`, giving RVA `0x6ce0`) and
disassembled it directly.

Traced the full logic: it first queries the real sensor status via a
synchronous-or-overlapped `DeviceIoControl(handle, 0x440010, ...)` (the same
GetStatus IOCTL seen everywhere in this document's traces), then branches on
the returned `WINBIO_SENSOR_STATUS` value (confirmed against this project's
own `winbio.h`: `WINBIO_SENSOR_READY=3`, `WINBIO_SENSOR_BUSY=4`,
`WINBIO_SENSOR_NOT_CALIBRATED=5` - this project's own
"BUSY→READY" patch produces exactly `3`, landing on the correct/intended
success path, not an edge case). On the ready path, it validates the
requested purpose against a bitmask (`WINBIO_PURPOSE_ENROLL=4` passes),
allocates an output buffer sized `0x1d418` bytes (**exactly** matching the
`out_size=119832` seen in every `0x440014` `DeviceIoControl` call throughout
this document), then issues `DeviceIoControl(handle, 0x440014 /*StartCapture
IOCTL*/, captureParams, 0x20, outBuf, 0x1d418, &bytesReturned, &overlapped)`
using the adapter's own embedded `OVERLAPPED` structure - and **returns
immediately** after that call, treating both an immediate `TRUE` return and
`GetLastError()==ERROR_IO_PENDING` (`0x3E5`) as "started OK, pending",
writing a pointer to its own `OVERLAPPED` back through the `ppOverlapped`
out-parameter. It does **not** itself wait for the capture to finish.

This matches `device.c`'s own already-correct handling exactly:
`tudor_enroll_capture` takes that returned `OVERLAPPED*`, wraps it via
`async_new_res`, and registers `enroll_cb` via
`winio_set_overlapped_callback` - i.e. this project's own overlapped-I/O
plumbing (`io.c`'s `GetOverlappedResult`/`winio_wait_overlapped`, already
reviewed above and confirmed correct) is what actually waits, and its
completion is tied 1:1 to the driver's own `WdfRequestComplete` for that
exact `0x440014` request - the same completion this document has traced
exhaustively from the other side (worker thread, watchdog, `0xc00000b5`).

**Conclusion: the Adapter DLL is not hiding a second, independent bug.** It
correctly issues one standard overlapped IOCTL and gets out of the way; all
real capture logic - and the entire missing piece - genuinely lives inside
`synaWudfBioUsb52.dll`'s own worker thread, exactly where this document has
been looking. This closes off the last major *unexamined surface* with a
clean, well-understood negative result, rather than leaving it as an
assumption.

**Also checked while here, closing two smaller open threads with hard
evidence**: (1) `CreateThread` is logged for *every* thread the driver ever
creates, regardless of target RVA (the logging isn't scoped to
`StartCapture`'s RVA) - grepped every trace this session and confirmed the
driver **never** creates a thread at any RVA other than `StartCapture`'s
own (`0x1c21c`), including during `OnPrepareHardware`/`OnD0Entry` (both of
which already exist, already run in the correct real PnP order, and both
already succeed silently every test this session - contrary to what this
document assumed further up before rereading `com_activate.c`'s own
`tudor_com_add_device` in full). So there is no separate,
early-initialized "continuous reader" thread anywhere to find. (2) This
project's own `CreateFileA`/`CreateFileW` + `SetupDi*` shim (`setupapi.c`) -
built specifically to let a driver discover a device path and
`WinUsb_Initialize` against it, the "normal" real-Windows prerequisite for
using `WinUsb.dll` - is real, working infrastructure from an earlier pass,
and its own comment already documents (and this session's traces
reconfirm) that it's never reached either. Every documented, standard UMDF
1.x USB-access mechanism this project's own code anticipates - COM
`IWDFUsbTargetDevice/Pipe`, flat WDF USB target, `GetDefaultIoTarget`,
`WinUsb.dll` via static or dynamic (`LoadLibraryA`) resolution, and
`SetupDi`+`CreateFile`+`WinUsb_Initialize` - is real, implemented, and
**confirmed live to never be reached**. The remaining blocker is now about
as tightly bounded as black-box reversing can get it: something inside
`synaWudfBioUsb52.dll`'s own worker-thread dispatch logic (the `ctx->field_0x100`
`+0x8`/`+0x240` fields from earlier in this document) decides, on its own,
never to progress far enough to attempt *any* of these - and pinning down
exactly why almost certainly needs either the real driver's own symbols/PDB
or a genuine Windows+hardware reference trace to compare against, neither
of which is available in this environment.

### Live-confirmed (not just statically inferred) that `field_0x240`'s consumer is never reached at all - and two more architectural dead ends

Fixed a methodology gap first: earlier live-GDB attempts in this session
were silently failing because `break <symbol>` was issued *before* `run`,
while `libtudor.so` (and the driver DLLs it loads) aren't mapped yet - GDB
was auto-declining "make breakpoint pending on future shared library load?"
in batch mode (`-batch` answers prompts with their default, "no", when
stdin isn't a terminal), so `run` proceeded to completion with **no
breakpoint ever active**, silently. Fix: `set breakpoint pending on` before
any `break`. Worth remembering for any future live session - this can look
exactly like "the code path was never hit" when it's actually "the
breakpoint was never armed."

With that fixed, went back to the one concrete, checkable claim from
earlier in this document: does the "prepare capture data" routine (RVA
`0xa0020`, which reads `ctx->field_0x100`'s sibling field `+0x240` via a
call through the embedded vtable's `+0xa0` slot at RVA `0xf8c40`) actually
run during a real capture attempt, or only in theory as part of the same
call graph? Computed the live driver base via the same proven technique as
elsewhere in this document (`base = $rdi - 0x1c21c` at a breakpoint on
`tudor_hook_thread_create`), armed breakpoints at `base+0xa0020` and
`base+0xf8c40`, and ran a full capture attempt (device-open's automatic
warmup `StartCapture`) through a complete watchdog cycle. **Zero hits on
either breakpoint.** This is a real, live-confirmed negative result, not a
static assumption: `field_0x240` and everything gated behind it is
downstream of the same "success path" this document already proved is
never reached (see "Found and confirmed the real success path" above) -
it's not an independent blocker, and further work chasing `field_0x240`'s
populator specifically is not worth pursuing further.

Also, while set up for live tracing, grabbed a real stack backtrace at the
worker thread's actual `WaitForSingleObject` call (breaking on this
project's own `win_wait_sync_obj`, which every `WaitForSingleObject` call
in the whole process routes through): confirms the caller's return address
is RVA `0x14e4b` in the driver DLL (`0x00007ffff5dd9e4b` at a live base of
`0x00007ffff5dc5000`) - matching, almost to the byte, this document's
existing static finding of the wait at RVA `0x14e41`. Good sanity check
that the RVA/base arithmetic and the identified call site throughout this
document have been correct; not a new lead by itself.

Two more angles checked and closed this pass, both concrete and
disprovable rather than speculative:
- **Traced what feeds the `0x440014` IOCTL's 32-byte capture-parameters
  buffer** in the Adapter DLL's `StartCapture` (the GUID-sized chunk at its
  center comes from a call through `WINBIO_PIPELINE::EngineInterface` at
  struct offset `0x38`, which is `WINBIO_ENGINE_INTERFACE::QueryPreferredFormat`
  per this project's own `winbio.h` layout) - this is the Adapter DLL
  calling a sibling real function inside its own DLL, not something this
  project fabricates or could get wrong; not a promising place to keep
  looking for a data-correctness bug.
- **Checked whether the driver ever creates a second WDF I/O queue**
  (distinct from the one devctrl queue already known) that might be meant
  to receive automatically-dispatched read data from the `UmdfDispatcher=WinUsb`
  reflector's own continuous-read machinery (`EvtIoRead`, which this
  project's `com_create_io_queue` doesn't wire up at all - only
  `EvtIoDeviceControl`). Live-confirmed: `CreateIoQueue` is called exactly
  once per device, always for the same devctrl queue. There's no second,
  unimplemented read queue silently being ignored.

At this point, essentially every concrete, checkable hypothesis this
session and the sessions summarized above it have been able to formulate
has been checked against live behavior and closed. The blocker remains
real but is now unusually well-bounded for a black-box reversing effort:
it's a decision made entirely within `synaWudfBioUsb52.dll`'s own worker
thread, upstream of every I/O mechanism this project implements, and
downstream of nothing this project's own code visibly controls incorrectly
(status query, purpose value, and capture-parameters construction have all
now been checked and look right). Further progress most likely needs
either the real driver's symbols, or a genuine Windows+hardware reference
trace (e.g. a USB packet capture or ETW/WPP trace from real hardware) to
diff against - not more blind disassembly of this same binary.

### External research: found a real Windows USB capture for this exact device, and a fixed real (independent, non-BCrypt-related) bug along the way

Per user direction, switched from more disassembly to external research:
does anyone else reversing this exact hardware have documentation, and does
a real reference trace exist anywhere?

**Found the origin issue for this whole fork**:
[`Popax21/synaTudor#36`](https://github.com/Popax21/synaTudor/issues/36) -
`todorz` (this fork's author) originally asked Popax21 (upstream synaTudor's
author) for help porting to `06cb:0082`/USB052; Popax21's guidance got the
project past `EncodePointer`/`GetProcAddress(NULL)` crashes, and everything
from that point through this entire document is `todorz`'s (and now this
session's) own subsequent work - no further public guidance exists past
that point.

**Found a much more directly useful reference**:
[`Popax21/synaTudor#61`](https://github.com/Popax21/synaTudor/issues/61)
documents a completely independent, *successful* port to a different PID
(`06cb:00bd`, a match-in-sensor "v105"-generation device) by a third party.
Not directly applicable (different driver generation, match-in-sensor vs.
this device's presumed match-on-host design), but it establishes a real
precedent for the *class* of bug that silently blocks these WBDI drivers
without crashing: **its CAPI symmetric crypto implementation
(`CryptEncrypt`/`CryptSetKeyParam`) was using a fresh cipher context per
call instead of persisting CBC chaining state across calls**, silently
corrupting a multi-call secure session handshake with the sensor - no
crash, just a handshake that could never complete, exactly this
investigation's symptom shape.

**Checked this project's own crypto for the equivalent bug, in the API this
driver actually uses.** `synaWudfBioUsb52.dll`'s import table includes
substantial `BCrypt` imports (`BCryptGenerateKeyPair`, `BCryptSecretAgreement`,
`BCryptDeriveKey`, `BCryptSignHash`) alongside legacy `ADVAPI32.dll` CAPI
(`CryptEncrypt`/`CryptSetKeyParam`/etc.) - this driver generation uses CNG,
not (only) legacy CAPI. Checked this project's `BCryptEncrypt`/`BCryptDecrypt`
(`winapi/bcrypt/bcrypt.c`) against real documented CNG behavior: for CBC/CFB
chaining mode, `BCryptEncrypt`/`BCryptDecrypt` are supposed to update the
caller's `pbIV` buffer in place after each call, so a caller can chain
multiple calls over a larger message using the same IV variable, without
managing a persistent cipher object itself (CNG's replacement for CAPI's
object-persisted chaining state - confirmed via Microsoft's own
documentation/community reports). **This project's implementation never did
this** - `iv` was passed through read-only into `aes_encrypt`/`aes_decrypt`
(which correctly builds a fresh `EVP_CIPHER_CTX` per call, matching CNG's
own one-shot-per-call model) but the *caller's* `iv` buffer was never
updated afterward. Fixed in `BCryptEncrypt`/`BCryptDecrypt`: after a
successful CBC/CFB operation, copy the last `iv_size` bytes of the
ciphertext (`out` for encrypt, the *input* `in` for decrypt - captured
before decrypting, since decrypt can be in-place) back into the caller's
`iv` buffer; GCM is deliberately excluded since its nonce/IV handling is
already special-cased elsewhere and doesn't chain this way. Compiles clean.

**This is a real, independently-justified bug fix - kept - but empirically
does not explain this session's blocker.** Live-traced a full capture
attempt with `BCryptEncrypt`/`BCryptDecrypt`/`BCryptOpenAlgorithmProvider`/
`BCryptGenerateSymmetricKey` all logged: **zero calls of any kind, to any
crypto API (CNG or legacy CAPI), anywhere in the traced session** - not
during device-open, not during the automatic warmup capture. Consistent
with (not contradicting) everything else in this document: the driver's
own initialization never gets far enough to attempt a secure session
handshake at all, the same way it never gets far enough to attempt real
USB I/O of any other kind.

**The single most valuable find of this research pass**: a real Windows 10
USB packet capture of this *exact* device (VID/PID `06cb:0082`), attached to
[`nmikhailov/Validity90#32`](https://github.com/nmikhailov/Validity90/issues/32)
(`mycapturefingerprint2.pcap.txt`, a `usbmon`-format `pcapng` capture,
downloaded and parsed directly with `scapy`'s `PcapNgReader` plus a manual
`usbmon_packet` struct decode - `tshark`/`capinfos` aren't installed in this
environment and couldn't be added without root). 81 URBs, decoded in full.
Key findings, now **ground truth**, not inference:

- The device exposes bulk endpoints `0x01`(OUT)/`0x81`(IN) as the **primary
  command/data channel**, plus a separate interrupt-IN endpoint `0x83`
  (confirmed independently by a `lsusb -v` dump posted in the same issue's
  comments: `bInterfaceClass=255` vendor-specific, 5 endpoints total,
  `EP1 OUT`/`EP1 IN`/`EP2 IN` bulk, `EP3 IN` interrupt 8 bytes).
- **The real capture protocol is a synchronous request/response exchange
  over the bulk pipe** - a short opcode byte (or a few bytes) written OUT,
  then a response read IN, repeated dozens of times per operation. The
  interrupt endpoint is essentially idle through the entire capture/upload
  sequence in this capture - armed once near the start, once again near the
  end - not a per-transfer completion signal.
- Observed opcodes in the capture include `0x39` (paired with two different
  ~125-byte parameter blocks across the trace - looks like the primary
  "start operation" command, varying by what's being started),
  `0x17` (sent 4 times in a row, each time getting back the *identical*
  7-byte response `00 00 01 00 80 00 10` - a clear polling loop, almost
  certainly a status/ready poll), `0x04` and `0x52` (short single-byte
  commands with 2-byte `00 00` acks), and large (4096/4928/1653-byte)
  bulk-OUT blocks that are very likely image or template data being
  uploaded to the sensor.
- **The `prototype` tool from the same repo** (`Validity90/prototype/main.c`)
  was run against real `06cb:0082` hardware by five different people in that
  issue's comments (2018-2022), all showing the **same fixed
  initialization sequence**: write `0x01` → read a 32-38 byte status block
  whose byte at offset 2 reports the sensor's init state (**`0x02` means
  initialized; every tester's fresh/unpaired unit reported `0x00` or `0x06`
  - "Sensor not initialized"**); write `0x19` → read a ~68-byte block
  containing what is very likely a per-session nonce/challenge (differs
  every run); write `0x06` followed by a ~250-byte payload (opcode `0x06`,
  4 parameter bytes `02 00 00 01`, then the same `0x39`/`0x17` bytes seen in
  the bulk-capture trace, then 240 bytes of high-entropy data) → read 2
  bytes back - this is almost certainly the ECDH public-key/signed-nonce
  exchange the `BCryptGenerateKeyPair`/`BCryptSecretAgreement`/
  `BCryptSignHash`/`BCryptDeriveKey` imports exist for; then `0x3e`
  (capabilities query) and further short opcode/response pairs.

**What this confirms, concretely**: this device genuinely requires a real,
non-optional pairing/provisioning handshake over bulk USB transfers - using
exactly the crypto primitives this driver imports - before normal capture
operations are meaningful, and this is **independently, externally
confirmed real hardware behavior**, not something inferred from this
project's own static analysis. Combined with this session's own exhaustive,
live-confirmed proof that no bulk transfer, no `WinUsb` call (static or
dynamic), and no crypto API call of any kind ever happens in this bridge,
this externally validates (rather than redirects) this document's core
conclusion: **the actual gap is that the driver's own logic never attempts
this real hardware conversation at all**, not a misdiagnosis of where to
look. It does not, by itself, reveal *why* the driver's internal state
machine declines to start - that remains the open question - but it
confirms the shape of what "working" looks like at the wire level, which
this document didn't have concretely before, and gives exact opcode values
(`0x01`, `0x19`, `0x06`, `0x3e`, `0x17`, `0x04`, `0x52`) that a future pass
could search for directly in the driver's own disassembly (as literal
bytes being written to an outgoing bulk buffer) as a fundamentally
different, externally-anchored search strategy from anything tried so far
in this document - not yet attempted.

Also worth noting for continuation: this device is not supported by
`python-validity` (the most actively maintained from-scratch Linux
Synaptics driver, `uunicorn/python-validity`) either - a from-scratch
reimplementation of the protocol (bypassing the Windows driver entirely,
the way `python-validity` does for the sensors it does support) is a
theoretically viable alternative path if the relinking approach's specific
blocker is never found, but is a substantially larger undertaking than
continuing to debug the relinked driver, and out of scope unless
specifically directed.

### BREAKTHROUGH: found and fixed the actual gate - `IWDFDevice::RetrieveDeviceName` was never implemented, silently skipping all of `OnPrepareHardware`'s real work

Acted on this document's own "not yet attempted" lead from the `field_0x240`
section above: live-traced `OnPrepareHardware`'s and `OnD0Entry`'s real
implementations directly (computed their live RVAs via a temporary
`log_debug` in `com_create_device`/`tudor_com_add_device`, then disassembled
`synaWudfBioUsb52.dll` at those addresses) instead of only knowing that they
run and return success.

`OnPrepareHardware`'s real implementation (RVA `0x223a8`) opens with a call
through `outer_self->field_0xd0`'s own vtable slot `+0x88`, passing a flag
(`0`) and an output pointer, and **gates all of its subsequent real work on
that output being nonzero** - if zero, it takes a WPP-trace-and-return path
instead (RVA `0x180022441` onward) and never reaches the code past
`0x18002253c`. Live-broke on the call site and captured both the "self"
object and the resolved target function - the target resolved to this
project's own `com_stub_17` (`info symbol` confirmed it's inside
`libtudor.so`, not the driver), and the log line for that stub hit
(`self=0x5555555e7570`, matching `device_obj`'s already-known identity from
many earlier `com_create_io_queue` correlations) confirms: **this is
`IWDFDevice::RetrieveDeviceName`** (slot 17, per this session's own verified
0-indexed WDK `wudfddi.h` count - `RetrieveDeviceInstanceId`=10,
`GetDefaultIoTarget`=11, ..., `CreateDeviceInterface`=15,
`AssignDeviceInterfaceState`=16, `RetrieveDeviceName`=17), called exactly
in the standard Win32 two-phase pattern (`pDeviceName=NULL` to query the
required length first). The generic stub answers `S_OK` without ever
touching `pdwDeviceNameLength`, which stays at the caller's own
zero-initialized value - **a silently "successful" call that reports "no
device name," not a crash, not a loud failure** - exactly the class of bug
this whole document has been looking for: something this project's own code
was skipping without any error signal.

**Fix** (`com_activate.c`): implemented `com_retrieve_device_name`
(`IWDFDevice::RetrieveDeviceName`, `device_obj->vtable[17]`) as a real,
two-phase implementation returning a plausible fake device path
(`\Device\0000TudorWBDI`) with a genuine nonzero length. Compiles clean.

**Live-confirmed this is the real gate**: with the fix in place, the log now
shows `com_retrieve_device_name` called *twice* - first with
`out_name=NULL, *out_len=0`, correctly returning `*out_len=22`; then again
with a real `22`-`WCHAR` buffer the driver itself allocated based on that
returned length. This is **new, previously-never-reached code executing for
the first time in this entire investigation** - concrete proof the fix
unblocks real forward progress, not just a plausible-looking change.

**Immediately after, hit a new crash** - `RIP=0x0`, a call through a NULL
function pointer, inside a reference-count-style "release" helper (RVA
`0xf1220`/`0xf1230`, live-confirmed via a raw stack scan for driver-range
return addresses, since naive frame-pointer-less unwinding gives garbage
frames past the first one - see this document's own methodology notes on
why). The object being released (`self`, a real non-null heap pointer) has
its first 4 QWORDs (`+0x0` through `+0x18`) entirely zero and only
`+0x20`/`+0x28` populated - i.e. this is the same general *shape* of bug as
the already-documented "real layout mismatch"/AddRef+lock recursion pattern
earlier in this document (a real driver object with a bound-callback shape
- data/context fields populated, function-pointer slot(s) still zero),
just on a new, not-yet-identified object reached for the first time via
this newly-unblocked path. Not yet resolved - this is now the immediate
next blocker, one clear layer past everything this document was stuck on
before this pass. Given the whole investigation was blocked on
`RetrieveDeviceName` for what looks like a very long time, it's plausible
(not yet confirmed) that fully resolving this new crash reaches
`CreateDeviceInterface`/`AssignDeviceInterfaceState` (slots 15/16, called
just before `RetrieveDeviceName` in the same sequence, still on the
generic stub) as the next thing needing a real implementation, and
potentially the real hardware-pairing handshake this document's external
research (the Windows USB capture, above) now shows is genuinely required.

### Follow-up: diagnosed the new crash further - it's generic smart-pointer teardown, and a real `CreateThread` call sits just past it

Traced further into `OnPrepareHardware`'s now-reachable real body
(RVA `0x223a8`-`0x22f1a`, a ~3KB function - previously never examined
past its first few instructions). It's substantial, real work: dozens of
sub-calls, and critically, at RVA `0x22db4` it calls through the IAT slot
for **`CreateThread`** (confirmed via the PE import table:
`0x18012d2b0` = `CreateThread`). This is - almost certainly - the exact
call Popax21's own comment earlier in this document refers to
("A `CreateThread` call was found statically inside
`CBiometricDeviceUSB::OnPrepareHardware`... live breakpoint tracing
disproved this - `CreateThread` is never actually reached from
`OnPrepareHardware` in practice"). That was true prior to this session's
fix; it is very plausibly no longer true now, but the new crash (below)
currently sits *before* this point in the function and hasn't yet been
confirmed reached live.

Also found, a few instructions after the `RetrieveDeviceName` gate itself:
real calls to `WideCharToMultiByte` and `lstrlenA` (RVAs `0x22848`/
`0x2286f`) - the driver converting the device name this project now
supplies from UTF-16 to ANSI and measuring it, i.e. genuinely consuming
the fake name - further confirming the fix is real, not coincidental.

**The crash itself**: the "release" helper at RVA `0xf1220`/`0xf1230` this
document identified above turns out to be an extremely generic utility -
grepped the whole disassembly for its call sites and found **over 90
callers scattered across the entire DLL** (via one further layer of
indirection, a `~90`-call-site wrapper at RVA `0x7c590`/`0x7c5d0`) - this
is almost certainly an inlined-everywhere C++ smart-pointer/`CComPtr`-style
destructor, not something specific to device interfaces or hardware setup.
So the crash isn't tied to one specific "device interface object" - it's
whatever *local* COM-smart-pointer variable happens to go out of scope
incompletely constructed, inside this newly-reached, very large function.
Confirmed reproducible (identical `self` pointer value and identical
all-zero-except-two-fields shape across repeated runs with the same
script) - not a heisenbug.

Attempted to trace the object's construction site by breaking on the
allocator (RVA `0xd88c0`) and logging every allocation up to the crash, to
find the one that produced this exact object - hit a real GDB tooling
limitation: `finish` inside a breakpoint `commands` block doesn't survive
the allocator being called reentrantly/very frequently (it stops on the
*next* hit of the same breakpoint instead of completing cleanly), and
this allocator is called far too often throughout normal startup to
practically single-step through by hand. Not resolved this pass - the
next concrete step is either a hardware watchpoint on the specific
allocated address (once one specific allocation is confirmed as the
right one) or a different breakpoint strategy that tolerates reentrancy
(e.g. a dedicated Python `Breakpoint.stop()` that ignores nested hits and
logs to a file instead of using `finish`).

**Deliberately did not attempt a binary patch to skip the crashing call**
(e.g. redirecting the call site through a null-checking trampoline, the
same technique this project's own `stub.c` already uses to synthesize
small executable stubs at runtime) - the object's incomplete-construction
root cause isn't understood yet, and masking the crash without knowing
*why* construction was incomplete risks silently proceeding with wrong
data rather than genuinely fixing anything, which would be far harder to
notice than a clean crash. Worth reconsidering once the construction site
is identified.

### Follow-up: fully traced the crash to its real construction site - it's a "PairingContext" singleton, confirmed from two independent call paths

Fixed the GDB tooling gap that blocked clean tracing earlier (`finish`
inside a `Breakpoint.stop()` doesn't survive a reentrant/frequently-hit
breakpoint - it stops on the *next* hit instead of completing): switched
to `nexti` (step-over-calls) from a breakpoint set at the known call site,
which stays at call-granularity without descending into allocator/libc
internals. Used this to walk the crash backward one call at a time -
`nexti` to the last RVA reached before `$pc` goes to `0`, then re-target
the breakpoint one level deeper and repeat - from the second
`RetrieveDeviceName` return all the way down through six more real,
substantial functions:

`OnPrepareHardware`'s "add this device's name to an internal list" logic
(RVA `0x22906`, itself downstream of the real `WideCharToMultiByte`/
`lstrlenA` calls already found) → a real "build a name-wrapper object and
prepend it to a linked list" constructor (RVA `0x45a30`) → a
lock/singleton-guard wrapper (RVA `0x5c1b0`) → another wrapper (RVA
`0x7bdf0`) → another (RVA `0x7c660`) → a **second, independent call site**
(RVA `0x7c702`, inside `0x1800ea590`) that turns out to lead into the
*exact same* "get-or-create singleton" function this document already
identified (`0x1800ea660`-`0x1800ea764`) - confirmed by the trace landing
on the identical RVA (`0xea6c5`, the `call 0x1800f1060` instruction) as
the original crash-finding pass. That function, and the raw
allocate-then-zero-then-store-a-growable-array constructor at RVA
`0xf1060` it calls, is fully disassembled above.

**This closes the loop with real confidence, not just a plausible
correlation**: two structurally different code paths in this newly-reached
part of `OnPrepareHardware` (one for the device name list, one via this
second call site) both funnel into constructing/fetching the *same*
global, reference-counted singleton object - and neither path, nor the
"get-or-create" function itself, nor the raw constructor at RVA `0xf1060`,
**ever writes anything to that object's `+0x8` field**. It's zero from
`calloc`-style zero-fill through to being installed as the global
singleton and having its refcount incremented - genuinely, provably never
set on this path, not something this session's tracing simply hasn't
found yet.

The nearby `.rdata` strings `"PairingContext"`/`"UnpairingContext"`
(found while investigating a *different*, real `IWDFNamedPropertyStore::
GetNamedValue("PairingContext", ...)` call earlier in this same function -
worth implementing `IWDFDevice::RetrieveDevicePropertyStore` for
separately, since it's real, currently-stubbed, load-bearing
infrastructure, but confirmed *not* the direct cause of this specific
crash) make it very likely this singleton represents the driver's
in-memory pairing/session state object - exactly the kind of object the
external research (Windows USB capture) above shows a real pairing
handshake would need to track across its multi-step exchange. Its `+0x8`
field is very plausibly meant to hold a callback into logic that only
becomes relevant once actual pairing traffic starts flowing - i.e.
plausibly fine to be `NULL` on every real Windows boot too, *unless* an
error-cleanup path runs before that traffic starts, which is exactly what
seems to be happening here (something this session hasn't yet identified
still fails earlier, triggering a premature release of an intentionally
not-yet-fully-populated object).

**Where this leaves things**: the crash is now fully understood down to
the exact construction site and exact missing field, from two independent
angles - a genuine, well-earned stopping point for this pass rather than
an arbitrary one. Two concrete forward options for a future pass: (1) find
what fails *earlier* that triggers the premature release (the more
correct fix, but requires more of the same painstaking backward tracing
this section just demonstrated works), or (2) implement
`IWDFDevice::RetrieveDevicePropertyStore` for real (a genuine, independently-
useful gap, since `IWDFNamedPropertyStore::GetNamedValue("PairingContext", ...)`
already gets called and currently silently returns nothing) in case a
correctly-populated stored `PairingContext` property changes which branch
gets taken before ever reaching this constructor.

### Follow-up: a THIRD recursion, found with a real finger touch

Automated (fingerless) testing above only exercises the driver up through the
"waiting for finger" prompt - it can't drive the actual capture logic that only
runs once a real finger is on the sensor. Testing with a physical touch (`e` →
finger index → real touch) surfaced a **new deadlock**, same shape as before
(main thread blocked in `WaitForSingleObject`/`thread_wait`/`pthread_join`,
a second thread blocked in `EnterCriticalSection`), but this time on a THIRD
object: a live backtrace + `info symbol` on the contended `sect` pointer
resolved it to **`g_fake_quine_obj_inner`** itself (the object
`g_fake_quine_obj.slot0` points at) - confirmed the driver calls
`EnterCriticalSection` directly on it, not just using it as an AddRef target.
Since `inner` never had a real `LockSemaphore` (it was only ever designed as a
refcount sink), this hung exactly like the original `g_fake_quine_obj` deadlock
did.

Naively adding a real mutex to `inner`'s own memory (leaving its `slot0` still
self-referential) fixed the deadlock but reintroduced the *original* corruption
one level over: `inner` is fed through the same AddRef+lock pattern too (as
"X" in its own separate invocation), so its own `slot0` needed the same
redirect-away-from-self treatment as `cs`, which in turn meant `g_fake_quine_obj`'s
route to `cs` (which had been going *through* `cs.slot0`) needed decoupling
from `cs`'s internal layout entirely.

**Final fix**: refactored into a reusable `struct lockable_quine` bundle
(`driver.c`) - each lock-capable object gets its own dedicated
`selfptr_router` (so `*(obj.slot1)` always resolves back to `&obj`,
independent of what `obj.slot0` is) and its own real recursive mutex; all
lock objects' `slot0` now point at one shared, terminal `g_fake_quine_obj_sink`
that nothing ever chases through further. `g_fake_quine_obj` (the outer,
`ctx->field_0x100` object) routes to `cs` via `cs`'s own `selfptr_router`
rather than through `cs.slot0`, so it no longer cares what `cs.slot0` points
at. Confirmed stable across repeated automated runs (reaches the finger-wait
prompt every time, no crash/hang) after this change.

### Follow-up: a FOURTH recursion, then a shared-landing-pad bug, then a real constructor

A second physical-touch test against the three-lockable-object fix hit a
**fourth** deadlock, on `g_fake_quine_obj_sink` itself (the object BOTH `inner`
and `cs` shared as their private `slot0` landing pad) - confirmed via the same
live-attach + `info symbol` technique. The mistake: `inner` and `cs` were
sharing ONE sink, so `inner`-as-X's AddRef and `cs`-as-X's AddRef both wrote
into the *same* memory, and that shared object was *also* independently used
as a direct lock target - reusing a landing pad across multiple parents lets
one parent's AddRef corrupt a value a completely different parent (or the sink
itself) depends on. Fixed by giving `inner` and `cs` each their own **private**
sink (`g_fake_quine_obj_inner_sink`, `g_fake_quine_obj_cs_sink`), both
`lockable_quine`s themselves, both bottoming out in a shared, plain
`g_fake_quine_obj_terminal` (accepting the same shared-landing-pad risk one
level further down, since by then depth had clearly bottomed out for the
warm-up path - automated testing reached the finger-wait prompt cleanly with
this shape across repeated runs).

Then a **third failure mode** appeared, not a deadlock or a data-corruption
SIGSEGV: `g_fake_quine_obj_terminal` (and separately `g_fake_quine_obj_cs_sink`)
turned out to be called as a **COM object with an embedded vtable**
(`call [obj+N]`) at at least two confirmed offsets, slot2 (0x10) and slot4
(0x20) - a self-pointer there jumps execution into the object's own data and
crashes outright. First attempt (making *every* non-reserved slot a callable
stub) backfired: some OTHER slot is read as *data* (a raw pointer passed
straight into `EnterCriticalSection`) by yet another consumer, and a stub
function's own code bytes reinterpreted as a `CRITICAL_SECTION` crashed
`pthread_mutex_lock`. Settled on making only the two concretely-confirmed
offsets (slot2, slot4) callable stubs, everything else stays a self-pointer
(`make_slots_callable` in `driver.c`).

**At this point the fake-object approach had accumulated four distinct,
sometimes-conflicting roles for the same handful of slots** (refcount target,
lock target, embedded-vtable call target, generic pointer-chase target) with
no obvious ceiling on how many more objects/offsets would need discovering.
Rather than continue, searched the whole driver DLL for real code that writes
to `[reg+0x100]` on a non-stack register (a candidate real constructor for
whatever `ctx->field_0x100` should point to) - see next section. **This is now
the more promising path and is very likely correct.**

### The REAL `ctx->field_0x100` constructor (major structural fix)

Found via `grep` over a full `objdump` disassembly for `mov [reg+0x100],reg2`
writes (excluding `rsp`/`rbp`-relative false positives): exactly one match, at
RVA `0xf6e00`, which - unlike `CTX260_CTOR_RVA` - does NOT allocate anything
itself. It's a pure vtable/field initializer: given a pointer to an
already-allocated buffer (its one argument, `rcx`), it writes real,
in-DLL `lea`-computed function pointers into offsets `0x70, 0x78, 0x80, 0x88,
0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0,
0xf8, 0x100, 0x108`, zeroes `0x110` (DWORD) and `0x240` (QWORD), and returns a
`uint32_t` status (0 = success, non-zero e.g. `0x6f` if its argument is NULL).
This is a genuine "placement new"-style object initializer for whatever real
type `ctx->field_0x100` is supposed to point to.

**Fix** (`driver.c`): `try_construct_real_field0x100_obj()` `calloc`s a
`0x300`-byte buffer (comfortably covering the highest offset touched, `0x240`)
and calls this constructor (RVA `0xf6e00`) on it; on success, `patch_device_context`
assigns the buffer's address to `ctx->field_0x100` instead of the fake quine
object, falling back to the quine only if the constructor's argument-null
check ever fires (shouldn't happen, we always pass a fresh non-NULL buffer).

This alone did **not** eliminate the fake-quine machinery - it's still needed
for one thing: the constructor doesn't touch offset `0x8` of its object at
all (that offset is outside its `0x70+` range), yet a separate, already-known
consumer dereferences `object->field_0x8` directly (the very first crash this
session root-caused, see "The `0x80070070` chain" above). So
`try_construct_real_field0x100_obj` also patches `real_obj->field_0x8 =
get_fake_quine_obj()` after a successful construction - reusing all the
lock/refcount/vtable hardening already built into the fake quine object, just
for this one narrower field instead of the whole outer object.

**Confirmed live**: switching to the real constructor immediately eliminated
the entire family of AddRef+lock deadlocks/corruptions/embedded-vtable crashes
above outright - capture with a real physical finger touch now proceeds
significantly further (through several more `EnterCriticalSection` calls on
*genuine* driver lock objects reached through the real vtable, not our fake
one) before hitting a **new, different** blocker (see below). This strongly
suggests the whole fake-quine saga was a symptom of using a synthetic stand-in
for an object the real driver has a perfectly good constructor for, and that
constructor should probably have been the very first thing tried once
`field_0x100`'s role was understood, rather than iterating on fake objects.

### Follow-up: the earlier "different object" was our OWN inner_sink, not a WDF COM object

The "param->field_0x10" `EnterCriticalSection` crash described above turned
out to be misdiagnosed - live-tracing the wrapper's *own entry point* (not
just its return address, which naive backtraces get wrong - see Methodology
notes) with symbol resolution on `rcx` showed the actual sequence of objects
touched: `g_fake_quine_obj_inner_sink`, twice successfully, then garbage. The
garbage was `g_fake_quine_obj_inner_sink.obj.slots[2]`'s own machine code
bytes (`0x81485657e5894855` = the literal opening bytes of a compiled x86
function) being read as a pointer and passed to `EnterCriticalSection` -
**not** a WDF COM `self` object at all. Root cause: `make_slots_callable` (the
fix for the embedded-vtable-call crashes on `terminal`/`cs_sink`) had been
applied *blanket* to every `lockable_quine`'s slots via `init_lockable_quine`,
but a *different* consumer double-dereferences `inner_sink`'s slot2 expecting
a plain **data** pointer there (confirmed live: this consumer's disassembly at
RVA `0x18007c0a3`/`0x18007c0a7` is `mov rax,[rax+0x10]; mov rcx,[rax]` - a
double dereference, not a call). Making slot2 callable on `inner_sink`
satisfied one consumer and broke a different one on the exact same object.

**Fix**: stopped blanket-applying `make_slots_callable`. It's now two
separate, narrowly-targeted functions (`make_slot2_callable`,
`make_slot4_callable`) applied only to the specific (object, slot) pairs
confirmed live to need it - `terminal` needs slot2; `terminal`,
`inner_sink`, `inner`, `cs`, `cs_sink`, and `g_fake_quine_obj` itself all
turned out (via repeated live retesting after each fix) to need slot4 as a
real callable stub, since a shared "Leave"-style call site reads slot4 on
whichever lock object it's given. Slot2 was confirmed conflicting on
`inner_sink` specifically, so it stays narrowly scoped to `terminal` only.

`g_fake_quine_obj` itself (not just its sub-objects) also turned out to need
a real mutex - confirmed live via `EnterCriticalSection` being called
directly on it (`sect == LockSemaphore == &terminal` was one variant found
along the way for `terminal` specifically; `g_fake_quine_obj`'s own slot4
embedded-vtable-call crash was the variant found for the outer object).
`terminal` got its own dedicated mutex (it isn't a full `lockable_quine` -
nothing observed needs `*(terminal.slot1)` to resolve back to `&terminal`,
only direct-call and direct-lock-by-address, so it didn't need the
slot0/slot1 treatment, just slot3).

### Follow-up: a double-free, then a use-after-free, then a real layout mismatch

Getting past all of the above (automated, fingerless testing now runs deeper
into the real driver's own object-lifecycle code than ever before) hit a
**heap corruption abort**: `free(): invalid size`, then (after fixing that)
`free(): double free detected in tcache 2`. Root cause, found via a `break
free` + `info symbol` live trace: the real driver calls `free()` directly on
`g_fake_quine_obj` (and, separately, on its own real heap-allocated objects
via this project's `HeapFree`) as part of its own COM-style AddRef/Release
object lifecycle (see the AddRef pattern documented earlier in this section) -
a static/BSS pointer was never a valid `free()` target.

**Fix, in two parts** (`driver.c`, `winapi/heap.c`):
1. The whole `g_fake_quine_obj` family switched from static globals to
   `calloc`'d heap objects, allocated fresh on every `get_fake_quine_obj()`
   call (removed the old cache-once-forever singleton pattern entirely,
   accepting a possible leak on cycles where the driver never releases its
   own reference, in exchange for not double-freeing a cached, already-freed
   pointer on a later cycle).
2. `HeapFree` (this project's own implementation, which the driver's
   statically-linked CRT free() ultimately routes through) now tracks freed
   pointers in a fixed-size open-addressing set and silently no-ops a repeat
   free of the same pointer, instead of calling libc `free()` again - this
   is a **generic** fix (any driver object, not just our fake ones) for a
   driver-side double-release bug this project's imperfect refcount/lifecycle
   emulation apparently triggers. Trades a possible leak (if an address is
   legitimately reused and its own later free also gets skipped) for not
   aborting the process.

With both of those in place, automated testing now reaches **yet another**
new crash: a write to `*(param->field_0x0) + 0x8`, where `param` is
confirmed live (via `info symbol` + reading raw bytes) to be the real
heap-allocated `ctx->field_0x100` object itself, and `param`'s first 8 bytes
are exactly the "type tag" (`0xca`) and "flags" (`0x8`) DWORDs the real
`FIELD0X100_CTOR_RVA` constructor deliberately writes there as two separate
32-bit fields (see "The REAL ctx->field_0x100 constructor" above). This
consumer instead reads those same 8 bytes as ONE 64-bit pointer and writes
through it - a genuine structural disagreement between two different pieces
of real driver code about what belongs at that object's offset 0.

### Follow-up: worked around the layout mismatch, reached real StartCapture processing

Tried to find the real caller of the crashing consumer (its true entry point
is RVA `0x49fd0`, found by scanning backward through `int3` padding from the
crash site) to understand the *correct* fix, but it has **no static callers
anywhere in the DLL** (not a direct `call`, not a `lea`-loaded address either)
- it's invoked through a raw data-table function pointer this project doesn't
have an easy way to locate short of scanning `.rdata` for the literal bytes
of its address. Also tried searching for code that checks the type tag value
(`0xca` = 202) specifically, hoping to learn the object's "real" type, but
`0xca` turned up as one of many literal comparisons in a large, unrelated
PNP/resource-descriptor type-dispatch table elsewhere in the DLL - a dead end
for this purpose (too generic a byte value to trace usefully this way).

Given the difficulty of finding the *correct* fix, applied a **targeted
workaround instead** (`driver.c`):

1. Two entries added to `g_driver_patches[]` (the same byte-patch mechanism
   already used for the BUSY→READY fix, now extended to support multi-byte
   patches via `memcmp`/`memcpy` instead of single bytes): NOP out the two
   `mov [rax+0x8],rcx` / `mov [rax+0x10],rcx` write instructions (RVA
   `0x4a11c`, `0x4a12a`) that crash when `rax` (the tag+flags-as-pointer
   value) is invalid.
2. More robustly, `try_construct_real_field0x100_obj` now **also** overwrites
   the constructed object's offset 0 with a pointer to the same hardened
   `get_fake_quine_obj()` used for offset 0x8 - instead of leaving the
   constructor's raw type-tag(`0xca`)+flags(`8`) bytes there. This makes
   `*obj` a genuinely safe, dereferenceable pointer for whatever consumer(s)
   expect one there, at the cost of losing the tag/flags values for
   whatever (if anything) reads them back as plain integers - not yet found
   any code path that does, for this specific object.

**Confirmed live**: with the offset-0 fix in place, automated (fingerless)
testing now runs dramatically further than at any prior point this session -
past the interactive "Enter finger index" prompt, into real `StartCapture`
processing (`IWDFDeviceInitialize-ish COM vtable slot #27` warnings appear,
never seen before), reaching an actual `memcpy`-style `rep movs` instruction
(RVA `0xfd949`) that crashes because its **source** pointer is NULL (`rsi=0`,
destination `rdi` is a plausible, valid-looking address, copy length `0x20`
bytes). This is a different, likely simpler class of bug than the layout
mismatches above - probably just another real field/object that needs
initializing (or another real constructor to find and call), not a
deep aliasing/refcounting issue. Not yet traced to its caller - same
methodology (find the true function entry via `int3`-padding scan or a
`tudor_hook_thread_create`-based live breakpoint, work outward) should
apply directly.

### Follow-up: traced fully to `ctx->field_0x168`'s GetOutputMemory - not slot 42, not a driver bug

Traced RVA `0xfd949`'s NULL-source `memcpy` back to its real caller (RVA
`0x18001abf7`, found via a raw stack scan for RVA-range values at the crash -
the naive backtrace was garbage/empty here). First hypothesis (COM vtable
slot 42 being unimplemented, since its log line's second argument happened to
match the copy length) turned out to be a **coincidence, not the real cause**
- disassembling the actual caller (RVA `0x18001d9f8`) showed it takes
`ctx->field_0x168` (this project's own devctrl request-facade pointer) and
calls straight through its vtable at slots `0x90/8=18` (`IWDFIoRequest::
GetInputMemory`, already implemented) and `0x88/8=17` (`GetOutputMemory`,
also already implemented) - both **real, already-working** implementations,
not stubs. So the missing piece isn't a missing COM method at all.

Re-ran with `-vv` (this project's CLI verbosity flag - `log_debug`/`[DBG]`
lines are suppressed by default at `LOG_INFO`) and added a temporary log line
to the previously-silent `com_request_get_output_memory`. This nailed it
precisely: the enrollment devctrl itself (`code=0x44200c`) is dispatched by
the real driver with `in_size=0 out_size=0` - **the real driver's own call
into `DeviceIoControl` genuinely requests a zero-size output buffer for this
specific IOCTL**, confirmed via the passthrough log line in this project's
own `DeviceIoControl` implementation (`winapi/io.c`), which logs the driver's
raw, unmodified arguments. `com_request_get_output_memory` correctly reflects
that (`out_buf=(nil) out_size=0x0`) - not a bug in this project's COM
implementation, just an accurate report of what the real driver asked for.
The driver's own later code then unconditionally copies 0x20 bytes from
whatever `GetOutputMemory`→`GetDataBuffer` returned, without checking that
the size is actually zero - the real inconsistency is on the driver's side
between what it requests via `DeviceIoControl`'s own `out_size` parameter and
what it internally assumes is available.

This lines up with the **already-documented, known limitation** noted
elsewhere in this file: `0x44200c` is the enrollment devctrl that originally
deadlocked and still relies on this project's generic timeout-based
`devctrl_watchdog` to force-complete it, rather than a real, driver-native
completion path (see `device.c`'s `devctrl_watchdog_thread` comment,
"StartCapture's completion machinery isn't fully replicated here"). It's
plausible the real driver expects to negotiate/allocate its own properly-sized
output buffer through some other channel entirely (not the `DeviceIoControl`
`out_size` parameter) before this second read happens, and this project's
enrollment path doesn't yet replicate whatever that mechanism is.

### Follow-up: fixed - it was GetInputMemory, not GetOutputMemory

First fix attempt (substituting a scratch buffer only in
`com_request_get_output_memory`) **did not** resolve the crash - same exact
crash PC (RVA `0xfd949`) before and after. Re-disassembled the actual caller
(RVA `0x18001d9f8`) more carefully and found the earlier read had the two
out-parameters backwards: the memmove's source (`[outer rsp+0x80]`) is
populated by the **input**-memory path (`call [rax+0x90]` = slot 18 =
`GetInputMemory`, then `GetDataBuffer` on the result), not the output path.
Devctrl `0x44200c` also has `in_size=0` (confirmed in the same `-vv` log,
right alongside `out_size=0`), so `com_request_get_input_memory` was
correctly returning a NULL buffer too - just not the one actually being read
from.

**Fix**: refactored both `com_request_get_input_memory` and
`com_request_get_output_memory` to share one helper
(`make_com_memory_obj_or_scratch`) that substitutes a zeroed `0x100`-byte
scratch buffer whenever the real buffer is NULL, applied to *both* input and
output defensively (since which side the real driver's own inconsistency
hits isn't something to assume in general). **Confirmed live**: the RVA
`0xfd949` crash is gone (verified via the crash PC changing on retest, and a
live register check confirming `rax`/`rcx`/`rdi`/`rsi` are no longer the
previous NULL-source pattern at all).

A **new** crash immediately follows: RVA `0x7c489` (`mov rax,[rax+0x8]`)
where `rax` is set from `[rsp+0x40]` (the function's own first argument) and
holds `0x55555558e` - a plausible **truncated** heap pointer (9 hex digits
where every other live heap pointer this session has been 12-13, all in the
same `0x555555......` range), not yet root-caused. Possibly a 32-bit/64-bit
truncation bug in a similar spirit to the `__winfnc`/ms_abi calling-convention
issues found earlier this project, or a genuinely different missing
initialization - needs its own live trace (same techniques: find the true
function entry via `int3`-padding scan from the crash site, breakpoint there,
inspect the full argument chain) to determine which.

### Follow-up: traced one level, hit an unreliable stack frame

Found the true entry point of the crashing function (RVA `0x7c410`, found by
scanning backward through `int3` padding) - it has 100+ static call sites
throughout the DLL (a very generic helper, likely a COM-object validity
check: the crashing read is `if(obj && obj->vtable) ...`-shaped). Live-traced
it directly (armed a breakpoint at its entry, matched on `rcx==0x55555558e`)
to its immediate caller: RVA `0x5d089`, a smaller wrapper (`if(obj) { ...
call the generic helper on obj ...}`) that itself has 22 static call sites -
still too many to pin down by inspection alone, and this wrapper receives
`obj` as a pure passthrough of *its own* first argument, so the "generic
COM-object checker" framing holds one level up too. The next frame up in the
live backtrace resolved to an address *below* the driver's own base (i.e.
outside the driver DLL entirely) - a naive-unwinder garbage frame (the same
pitfall noted in Methodology notes below), not a real caller.

`0x55555558e` itself doesn't obviously match any pattern from a truncated
64-bit heap pointer against the `0x5555555.......` addresses seen everywhere
else this session (no clean bit-shift or masking relationship found) - stayed
an open question rather than being resolved by inspection.

### Follow-up: root-caused - glibc tcache safe-linking, not a truncated pointer at all

`0x55555558e` turned out not to be corrupt-on-arrival at all. Live-traced
`get_fake_quine_obj()`'s own return value (breaking at its `ret` instruction,
computed directly since the function is a local/static symbol gdb can't
resolve by name pre-load - same `base + file_offset` technique as the driver
DLL, but against **libtudor.so's own mapping** instead) and confirmed
`g_fake_quine_obj->slots[0]` is set correctly at construction time (a normal,
valid heap pointer to `g_fake_quine_obj_inner`). Armed a hardware watchpoint
on that exact memory and caught the actual write: **`writer PC` fell inside
`libc.so.6`**, not driver code or this project's own code at all.

This is glibc's **tcache "safe linking"**: when a chunk is freed into the
tcache/fastbin freelist, glibc (≥2.32) doesn't store the raw "next free
chunk" pointer in the freed chunk's first 8 bytes - it XORs it with
`(chunk_address >> 12)` as a corruption-detection mitigation. For an empty
freelist (`next == NULL`), the stored value is just `chunk_address >> 12` -
which is *exactly* `0x55555558e390 >> 12 = 0x55555558e`, matching the
"truncated pointer" observed live to the last hex digit. In other words:
**the real driver actually called `free()` on `g_fake_quine_obj` and it
actually succeeded** (this was *before* the `0x8009800B`-era single-free
tracking existed to intercept it), and glibc's own freelist bookkeeping
overwrote `slots[0]` as an ordinary consequence of the chunk being freed -
this project's own `real_field0x100_obj->field_0x8` (see "The REAL
ctx->field_0x100 constructor" above) still held a reference to that same,
now-freed-and-reused memory and read through it later: a textbook
use-after-free, not memory corruption from an unrelated cause.

**Fix** (`winapi/heap.c`): `HeapFree` is now a **permanent no-op** - it never
calls libc `free()` at all, for anything the driver allocates via
`HeapAlloc`/`HeapFree` (superseding the earlier double-free-tracking fix,
which only masked the *second* free and left the *first*, chunk-corrupting
one intact). This trades unbounded memory growth for the life of the process
for never again invalidating memory something else still depends on -
entirely acceptable for a short-lived CLI test/debug run.

**Confirmed live, repeatedly**: with this fix, automated (fingerless) testing
no longer crashes *at all* through the entire `tudor_open` → interactive menu
→ `enroll` → finger-index → capture-attempt sequence. It reaches the real
capture logic and fails **gracefully**, exactly as expected with no physical
finger present: `[ERR] Error capturing sample for enrollment!` /
`[INF] Discarding enrollment of guid=...`, then cleanly returns to the
interactive menu. This is the correct, intended behavior for a "no finger on
sensor" condition - not a bug. **This is a major milestone**: the entire
driver bridge - from PE loading through WDF/COM emulation through real
`StartCapture` dispatch - now runs end-to-end without crashing, hanging, or
corrupting memory. The only thing left to determine is whether an actual
physical finger touch produces a successful capture and completed enrollment.

### Next step

**Test with a real physical finger touch.** Every crash/hang/corruption bug
found and fixed this session was in the bridging/emulation layer, not in the
real, unmodified driver logic itself - with all of them now resolved, a real
finger touch during `StartCapture` should be the first genuine end-to-end
test of whether the underlying sensor capture and enrollment logic itself
works correctly. If it still fails with a real finger, the failure mode
(error code, log output, or a new crash) will be much more informative now
that it's reached with a clean, stable bridge underneath it.

## Session continuation: real finger touch tested (multiple times) - confirms the watchdog fires completely independent of hardware state

Continuing from the "Next step: test with a real physical finger touch"
milestone above. Before running the actual test, chased one more dead end
first: hypothesized that `OnPrepareHardware`'s own `CreateFileA`/`WinUsb_
Initialize` call (using this project's own `RetrieveDeviceName` string,
`"\Device\usbpdo-0"`) failing with `INVALID_HANDLE_VALUE` (surfaced as
`0x800700d2`) was a real blocker, and made `setupapi.c`'s `CreateFileA`/`W`
accept that path too. **This was a regression, not a fix**: making that call
succeed pushes `OnPrepareHardware` down a real, previously-never-exercised
`WinUsb_Initialize`/pipe-claiming path (confirmed live: `WinUsb_Initialize:
claimed interface 0, 5 pipes`) that hits a brand new SIGSEGV shortly after,
in code this session's "major milestone" build never had to run. The
"Architectural clue... `UmdfDispatcher=WinUsb`" section above already
established this driver's INF-selected access model is
`GetDefaultIoTarget`/raw-IOCTL, not `WinUsb_Initialize` - the `0x800700d2`
failure is an expected, harmless probe rejection, not something to fix.
Reverted (`setupapi.c` only recognizes the original `FAKE_DEVICE_PATH`
again); rebuilt; reconfirmed the crash-free graceful-failure milestone still
holds unmodified.

**The real test**: ran a real enroll attempt (`e` -> identity `1` -> finger
`1`) three times with an actual finger physically on the sensor - once
briefly, once with `-vv` full verbose logging, once more with sustained
contact held through the entire capture window and per-line wall-clock
timestamps (`awk '{print systime(), $0}'`) to measure real elapsed time.
**Every single time, the result was identical to the fingerless case**:
`[ERR] Error starting capture: 0xc00000b5!` / `[ERR] Error capturing sample
for enrollment!` / `[INF] Discarding enrollment...`, no crash.

The verbose trace shows the full real chain for a `StartCapture` devctrl
(code `0x440014`): input/output memory handed to the driver, `CreateThread`
for the real worker thread (RVA `0x1c21c`, matching everything already
mapped above), the real `ctx+0x100`/`ctx+0x1b8` factories both returning real
objects (not fallback-fake-quine paths - the earlier structural fixes are
holding up), then several calls into the "fake StartCapture-context object"
(`driver.c`'s `g_fake_ctx_obj`, slots #2 and #4 - notably one `self=(nil)`
call into slot #4, worth checking in a future pass whether that's a genuine
null-context callback pattern or a facade/argument-count mismatch), and then
`WdfRequestComplete(flat) ... status=0xc00000b5` - all without ever calling
`com_get_default_io_target`, `com_create_interrupt`,
`get_usb_target_factory_facade`, `CreateFileA/W`, `WinUsb_*`, or any BCrypt/
CAPI crypto function. Same as every prior trace in this document.

**The timestamped comparison is the conclusive part.** Baseline (no finger,
two occurrences - the automatic warmup capture during `tudor_open`, then the
enroll attempt): both took *exactly* 5 seconds from the `StartCapture`
devctrl dispatch to `WdfRequestComplete(..., 0xc00000b5)` - this project's
own `devctrl_watchdog_thread` (`device.c`, `deadline.tv_sec += 5`) firing on
schedule, not the driver. Real-finger run, sustained contact held through
the whole window: **also exactly 5 seconds, to the same second.** This is
the same result the earlier "Conclusive test: extending the watchdog to 60s
with a real sustained finger touch" experiment already found, now
independently reconfirmed on top of everything fixed since then (the tcache/
`HeapFree` fix, the real `ctx->field_0x100` constructor, the layout-mismatch
workaround, `RetrieveDeviceName`, `WideCharToMultiByte`) - none of those
later, genuinely substantial fixes changed this outcome even slightly. The
driver's own logic makes zero observable decision based on physical sensor
state within this bridge; only this project's own timeout ever resolves the
request.

**Where this actually leaves things**: this is not a crash, hang, or
resource leak - it's a clean, deterministic "the real driver's capture logic
is simply never reached with genuine hardware state" result, exactly
matching the external USB-capture research above (zero bulk transfers, zero
crypto calls, ever). The single most concrete, externally-anchored lead not
yet acted on is that same research's ground-truth opcode sequence
(`0x01` init-check -> `0x19` nonce -> `0x06`+ECDH handshake -> `0x3e`
capabilities -> `0x17` poll, all over bulk endpoints `0x01`(OUT)/`0x81`(IN))
- a future pass should grep the driver DLL's disassembly for these literal
bytes being written to an outgoing buffer (not yet attempted at any point in
this document) to find where the driver *would* issue them, and from there
work out what specifically it's waiting on before doing so. The alternative,
much larger-scope path - implementing the real capture protocol directly via
`libusb` against this project's own bridge (bypassing the relinked driver's
internal, seemingly unreachable signaling entirely, reusing only its
higher-level enrollment/template/matching logic) - remains available if the
disassembly search doesn't pan out, per the "external research" section's
own note on this.

**Correction - the three "real finger" tests above were not actually
coordinated in real time.** The capture window is only 5 seconds, and the
user was asked to touch the sensor via a chat message *after* the process
had already started, with no confirmation the touch actually happened
within that window - almost certainly all three were, in practice,
fingerless runs that happened to agree with the fingerless baseline by
construction, not by coincidence. Caught by the user directly ("wait I
didn't use my finger at all you didn't notify me").

**Redone properly**: temporarily widened `device.c`'s devctrl watchdog from
5s to 30s (`deadline.tv_sec += 30`), started the run, waited for the actual
`Put your finger on the sensor` log line, and only then asked the user to
touch - who confirmed placement in-chat at essentially the same real-world
moment the window opened (`Put your finger on the sensor` and the user's
"my finger's on it now" landed within the same wall-clock second in the
timestamped log). Finger held for the full window. **Result: identical
`0xc00000b5` after exactly 30 seconds** - same outcome as every prior test,
but this is the first one with a genuinely verified, real-time-confirmed
physical touch behind it. Watchdog reverted back to 5s afterward.

**Methodology lesson for continuing this**: a chat-message request to
physically touch hardware is not synchronized with a 5-second (or even
30-second) automated window by default - either lengthen the window
substantially and wait for explicit real-time confirmation the touch has
happened before trusting the result (as done here), or find another way to
positively confirm contact (e.g. a libusb-level check of the interrupt
endpoint, if one existed) - do not report a "real finger" result without
one of these.

### Next step

Grep `synaWudfBioUsb52.dll`'s disassembly for the literal opcode bytes
(`0x01`, `0x19`, `0x06`, `0x3e`, `0x17`, `0x04`, `0x52`) being written to an
outgoing bulk-transfer buffer, to find the real code path that issues them -
this is the one concrete, externally-anchored search strategy from the
external-research pass that was never actually carried out. If found, trace
backward from there to whatever gates reaching it (the same live-tracing
techniques used throughout this document apply directly).

## Methodology notes (useful for continuing this)

- **RVA translation for live breakpoints**: the driver DLL isn't mapped until
  `tudor_init()` runs mid-process, so static addresses can't be used directly in a
  gdb script armed from `run`. Reliable pattern: set a pending breakpoint on
  `tudor_hook_thread_create` (a real exported symbol, resolves fine), and inside
  its `commands` block, compute `base = $rdi - 0x1c21c` (since `$rdi` there holds
  the worker thread's start_proc, always RVA `0x1c21c`), then
  `break *(base + target_rva)`.
- **Calling convention**: this codebase's Windows-ABI-facing functions use
  `__winfnc` = `__attribute__((ms_abi))`. Any function pointer typedef used to call
  *into* real driver code must have this, or arguments land in the wrong registers
  (SysV RDI vs ms_abi RCX for the first parameter) — this can silently "work" if
  leftover register state happens to be usable, then fail unpredictably later.
- **Static disassembly pitfalls**: `objdump` linear disassembly can start
  mid-instruction if the given start address doesn't align with real code (shows
  as garbage/invalid opcodes for the first few lines) — always sanity-check the
  first several instructions look real before trusting a disassembly window.
  Also watch for reading past a function's `ret` into the next, unrelated
  function's bytes when a window is too wide - confirmed to have caused at least
  one wrong conclusion this session (see "Dead ends" above).
- **GDB quirks encountered**: `set breakpoint pending on` is required for any
  breakpoint on a driver-DLL symbol/address armed before `run`. Named-parameter
  expressions (e.g. `status`, `slot`) are often unavailable ("No symbol table is
  loaded") at post-prologue breakpoint addresses in this codebase's build — use
  raw registers instead. A live internal GDB bug
  (`internal-error: Unexpected pending_follow.kind 14`) was hit when a hardware
  watchpoint was left armed across a new thread's creation — avoid holding
  watchpoints across thread-spawn boundaries.
- **Tooling**: no decompiler (Ghidra/IDA/r2) was available in this environment.
  `pacman -S radare2` would work but wasn't installed (would need root, not
  attempted). A Python venv with `capstone` was set up for programmatic analysis,
  but linear-sweep disassembly desyncs on embedded data — `objdump`'s own
  disassembly (which handles this correctly) piped through `grep` for call-target
  cross-referencing was more reliable for whole-DLL searches (e.g. "who calls RVA
  X").
- **Getting a backtrace on a hung (deadlocked) process**: the passwordless sudo
  grant only covers `/usr/bin/gdb` itself, not `kill`/`pkill` - so a plain
  `kill -INT` on a separately-launched `sudo gdb` process fails with "a password
  is required", and running the CLI directly (without gdb) to attach to later
  also fails the same way. The working approach: launch under `gdb -batch`
  as usual, but have the gdb *script itself* self-interrupt via Python -
  `python` block with a `threading.Timer`/background thread that sleeps N seconds
  then calls `os.kill(os.getpid(), signal.SIGINT)` - placed *before* the `run`
  command. When it fires, `run` is interrupted and the script continues to its
  next line (e.g. `thread apply all bt`), giving a real all-thread backtrace of
  a genuinely hung process without needing any extra permissions.
- **Reading a `objdump` backtrace address that turns out to be garbage**: naive
  frame-pointer-less unwinding (as GDB does by default on this PE code, which
  lacks the CFI/unwind info GDB's unwinder expects) can produce frames that are
  just stray stack *values* misread as return addresses - confirmed this
  session: a "frame" that resolved to this project's own `g_fake_quine_obj`
  global variable's address, which obviously never called anything. Don't trust
  a frame blindly; sanity-check it resolves to something that could plausibly be
  mid-call (e.g. immediately after a `call` instruction in the disassembly at
  that address) before reasoning about it further.
- **Finding who corrupts a fake object's memory - hardware watchpoints**: when a
  live crash's faulting pointer looks like a *slightly mutated* version of a
  known-good value (e.g. a self-pointer with one byte incremented), don't guess
  from static disassembly alone - arm a GDB hardware watchpoint
  (`gdb.Breakpoint(expr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)` in a Python script)
  on the exact live address once it's known (from `nm` on the built `.so` +
  the live mapping's file-offset base, same technique as RVA translation
  above). The watchpoint's stop handler gets a real backtrace at the exact
  write, far more reliable than reasoning backward from the crash site alone.
  This is how the triple-indirection AddRef+lock pattern in the "Enrollment
  hang" section above was actually found, after two rounds of
  static-disassembly-only fixes each only got one level deeper before hitting
  the same corruption again one level further in.
- **Getting privileged runtime for a plain (non-debugging) test run**: the
  passwordless sudo grant only covers `/usr/bin/gdb`, not the CLI binary
  directly, and the CLI needs root to open the USB device before it drops
  privileges. Running `sudo gdb -batch -x <script-with-just-"run"> --args
  ./tudor_cli <store>` gets a normal full-output run without an interactive
  sudo password prompt.

## BREAKTHROUGH: real USB hardware communication happening for the first time ever - via a path this document previously (wrongly) dismissed

### The wrong turn, and why it was actually right

Re-tested the fingerless-milestone build with a real, *properly time-coordinated*
physical finger touch (see "Session continuation" section above for how the
first three "real finger" tests turned out not to be real touches at all -
caught by the user). Widened the devctrl watchdog to 30s temporarily, waited
for the live `Put your finger on the sensor` log line, and only then asked
for a touch, confirmed in-chat at the same wall-clock second. **Result:
identical to every prior test - `0xc00000b5` after exactly 30s, regardless of
genuine, verified physical contact.** This independently reconfirms (on
today's fully-stabilized build) the standing conclusion from earlier in this
document.

Given that, and since the user asked to keep pushing rather than accept this
as a ceiling, went back to `OnPrepareHardware`'s own `CreateFileA`/
`WinUsb_Initialize` call - previously made to succeed by also accepting
`FAKE_WDF_DEVICE_NAME` ("\Device\usbpdo-0") in `setupapi.c`, then **reverted**
because it exposed a new SIGSEGV past `WinUsb_Initialize: claimed interface
0, 5 pipes`, reasoned (from the INF's `UmdfDispatcher=WinUsb` directive
alone) to be a wrong path since the "real" model was assumed to be
`GetDefaultIoTarget`/raw-IOCTL instead.

**That reasoning was wrong, or at least incomplete.** External research this
pass found [`MarcelineVPQ/elitebook840-fingerprint`](https://github.com/MarcelineVPQ/elitebook840-fingerprint),
an independent port of a *sibling* Synaptics Tudor/WBDI-family driver
(`06cb:00f0`, a different "110"-generation, match-in-sensor driver, via the
same synaTudor-style relinking approach) that reached real pairing, a live
TLS channel, and real secure-DB reads. Its own `docs/FINDINGS.md` documents
exactly the same shape of blocker this document has been fighting - real
communication requires calling real Windows APIs the shim didn't provide -
and explicitly describes real `libusb` bulk reads/writes actually happening
once those gaps were filled (`SYNA_USB_TIMEOUT` was added specifically
because an *infinite* real bulk read was hanging, not because none ever
fired). **The pattern this whole document has repeatedly proven - "untested
path -> new crash" means "next real bug to fix", not "wrong path" - applies
here too and was ignored on a purely static, unverified inference.** Reverted
the revert: `setupapi.c` once again accepts `FAKE_WDF_DEVICE_NAME`.

### Found and fixed the real crash: a second, independent instance of the "untouched out-param" bug family, on a not-yet-recognized interface

Live-traced the SIGSEGV past `WinUsb_Initialize` (same crash address as
before, `[rsp+0x68]` dereferenced as a pointer): a chain of two vtable-slot-3
calls, the first succeeding and populating (supposedly) an out-param that the
second call then dereferences. Identified the first call's `self` object
live: a **generic, untagged alias facade** (every slot still a
`com_stub_N`) - i.e. `device_obj` was `QueryInterface`'d for an IID this
project didn't recognize, got a blank fallback alias back, and the driver
then called slot 3 on it expecting a real method with a real out-param.

Getting the actual IID took a second, independent discovery along the way:
**this project's own `QueryInterface` miss-logging has a real, previously
undiscovered bug** (`com_query_interface`'s `log_debug`, `com_activate.c`) -
it prints the GUID struct's raw in-memory bytes directly, but `windows.h`'s
`DEFINE_GUID` macro only pre-corrects `PartD`/`PartE` (the last 8 bytes) for
x86's little-endian storage of multi-byte integers, not `PartA`/`PartB`/
`PartC` (the first 8) - so every previously-logged "for IID ..." string in
this project's history has had its first 8 bytes silently byte-reversed
per-field relative to the canonical display order. Not a functional bug
(`guid_eq`'s `memcmp` compares raw bytes correctly regardless), only a
debug-display one - but enough to send a naive lookup of the logged string
nowhere. Reversing `PartA` (4 bytes) and `PartB`/`PartC` (2 bytes each) by
hand turned the logged `067ebe45-659b-4d43-a7d6-9572d7f73d53` into
`45BE7E06-9B65-434D-A7D6-9572D7F73D53`, which matched immediately against a
real WDK header mirror ([`wmliang/wdk-10`](https://github.com/wmliang/wdk-10/blob/master/Include/wdf/umdf/1.11/wudfddi.h))
- fetched directly since this environment has working outbound network
access (confirmed live this pass; not previously known/used) - as
`IID_IWDFPropertyStoreFactory`.

`IWDFPropertyStoreFactory::RetrieveDevicePropertyStore` (real signature:
`RootSpecifier, Flags, DesiredAccess, SubkeyPath, **PropertyStore,
*Disposition`) matched the live call site's arguments exactly (`rdx`=struct
pointer, `r8`=0, `r9`=1, stack args NULL and the crashing out-pointer).
Implemented in `com_activate.c`: `com_query_interface` now recognizes
`IID_IWDFPropertyStoreFactory` on `device_obj` and returns a small factory
facade whose slot 3 (`com_property_store_factory_retrieve`) allocates a
persistent property-store object backed by the same `com_propstore_
get_named_value` (always "not found"/`VT_EMPTY`) already used by `IWDFDevice::
RetrieveDevicePropertyStore` - the same bug class as `RetrieveDeviceName`/
`RetrieveDeviceInstanceId` before it: a real out-param the generic stub never
touched.

**Live-tested: the SIGSEGV is completely gone, and real USB hardware
communication happens for the first time in this project's entire history.**
`WinUsb_ReadPipe` now returns genuine data over multiple real bulk transfers
during `OnPrepareHardware` alone: 38, 68, 2, 6, 10, 922, 10, 2, 2154 bytes on
endpoint `0x81`, a **51224-byte** read on endpoint `0x82` (a third bulk
endpoint - matches "5 pipes" claimed at `WinUsb_Initialize`), and a 5-byte
read on the interrupt endpoint `0x83` - closely matching the external
packet-capture research's documented real protocol shape (short opcode/
status exchanges, a large data block, the interrupt endpoint touched near
the start). A real `HKEY_LOCAL_MACHINE\SOFTWARE\Validity\Dbg` registry read
also happens in the middle of this, suggesting genuine driver-internal debug
config checking, not this project's own code.

Getting this far also required one more real gap, of the exact kind
`elitebook840-fingerprint`'s own findings predicted: an unresolved `USER32.dll`
import, `SetWindowLongPtrW` (this driver, like the HP one, runs a hidden
"power state window" for notifications). This project already had a
substantial `winapi/window.c` (`RegisterClassExW`, `CreateWindowExW`,
`DefWindowProcW`, a real per-thread message queue with `GetMessageW`/
`PeekMessageW`/`DispatchMessageW`, `WTSRegisterSessionNotification`) from
earlier work, just missing `GetWindowLongPtrW`/`SetWindowLongPtrW`/
`CallWindowProcW` - added, backed by a real per-window `GWLP_USERDATA` slot
and `GWLP_WNDPROC` subclassing support (everything else logs and returns 0,
matching this project's generic-stub convention elsewhere). Compiled clean,
fixed the abort.

### Current state after this fix

A full fingerless enroll attempt now: opens the device, runs a real
USB-level init/pairing-shaped conversation with the physical sensor during
`OnPrepareHardware` (dozens of real bulk transfers, not zero), reaches `Put
your finger on the sensor`, and **still** ends in the same `0xc00000b5`
devctrl-watchdog discard as before - but now with a genuinely, newly-
initialized sensor underneath it, which no test in this entire document has
ever had. All of the real communication observed so far happens during
device *open*, not during the `StartCapture` devctrl itself - the worker-
thread-never-completes blocker this document spent so long on may or may not
be the same story now that the sensor has actually been talked to for real.

### Tested with a real, properly-coordinated physical finger touch on top of this fix

Widened the devctrl watchdog to 30s again, waited for the live `Put your
finger on the sensor` line, asked for a touch confirmed in-chat at the same
wall-clock second (timestamped log: prompt and touch both at the same
second), held for the full window. **Result: `0xc00000b5` again, after
exactly 30s - and zero `WinUsb_ReadPipe`/`WritePipe` calls anywhere between
the prompt and the timeout.** All the new real USB activity from this fix
happens during device *open* (`OnPrepareHardware`'s own init/pairing-shaped
conversation); the `StartCapture` devctrl path itself still never issues any
real I/O, confirming the "worker thread never completes" blocker documented
exhaustively earlier in this file is a genuinely separate, still-unsolved
problem from the one just fixed - the newly-unlocked object graph
(`OnPrepareHardware`'s own WinUsb-backed interface chain) and the
`StartCapture` worker thread's own object graph (`ctx->field_0x100`, its
`+0x8`/`+0x240` sub-fields) are not visibly connected; nothing suggests the
former now feeds the latter. Watchdog reverted back to 5s.

**Net assessment**: this is still a major, durable milestone - genuine two-
way USB communication with the physical sensor now happens for the first
time in this entire project's history, and does so without crashing - but
enrollment itself is not yet unblocked. The `+0x8`/`+0x240` gap remains the
most precisely-identified next target (see "Found the real construction
path for ctx->field_0x100" above); whether `OnPrepareHardware`'s newly-real
conversation has *already* written real state somewhere that `+0x8`'s real
consumer chain could reach (rather than needing a whole separate real
conversation of its own) has not yet been checked and is the most promising
next thing to look at, given how much closer to real hardware state this
session's fixes have gotten.

### Next step

Check whether `OnPrepareHardware`'s newly-real WinUsb conversation populates
any state reachable from `ctx->field_0x100->field_0x8`/`+0x240` (the two
still-NULL fields identified earlier as the real remaining gate on
`StartCapture`'s worker thread) - if the driver already talked to the
sensor once during open, some of what `+0x8`'s consumer chain wants might
already exist somewhere in memory, just not wired to where it's read from.
If not connected, the next distinct object graph to trace live (same
techniques as `ctx->field_0x100` above: search for the real constructor's
callers, tag-dispatch validation, etc.) is whatever object
`OnPrepareHardware`'s own `IWDFPropertyStoreFactory`/`WinUsb` chain
constructs, to see if the driver keeps a persistent handle to it anywhere
`StartCapture` could also reach.

## Found a THIRD, even-outer real factory layer for ctx->field_0x100 - real data appears at field_0x8 for the first time, but shaped wrong for its known consumer

Acted on the "next step" above from the opposite direction first: searched
for callers of `FIELD0X100_DISPATCH_RVA` (`0xa0fc0`, the type-tagged
dispatcher this project already calls directly) instead of chasing
`OnPrepareHardware`'s object graph. Found exactly **one** real caller
anywhere in the DLL (RVA `0x9ef28`), inside an even larger function (real
start RVA `0x9ede0`) that is itself only called from two places, both
passing a type-tag field read out of a caller-owned struct (not a hardcoded
constant) - i.e. this is a **third, generic, even-more-outer real object
factory**, of which the `0xa0fc0` dispatcher this project already uses is
just one inner step.

This outer factory validates the type tag against a much broader whitelist
than `0xa0fc0` alone suggested (`0x65`/`0x66`/`0x67`/`0xc9`/`0xca`/a
`0x190`-`0x1be` range/`0x1f5` - `0xca`, this object's own tag, is one case
among many, not a coincidence), does its own real `0x260`-byte allocation
(this project was pre-allocating its own `0x300`-byte buffer and skipping
this - same "skipped a whole real layer" pattern as the `0xa0fc0` discovery
before it), zeroes it, calls `0xa0fc0` on it, and - the actual new
information here - **on success, calls the freshly-constructed object's own
embedded-vtable slots `+0x70` and `+0x80`** before ever handing it back to
its caller. `+0x70` takes a second argument (a zeroed 8-byte scratch buffer
at both real call sites, not a preexisting object); `+0x80` just gets a
fixed mode flag (`1`). Both are real, WPP-traced, non-stub code.

**Rewired `try_construct_real_field0x100_obj` (`driver.c`) to call this
outer factory instead of `0xa0fc0` directly**, providing a zeroed 8-byte
local as the `+0x70` scratch argument (matching both real call sites) and
its own `void*` out-param. Compiled clean, live-tested:

- **`ctx->field_0x100->field_0x8` is non-null for the first time in this
  entire project's history** (previously always zero from every construction
  path tried) - a real, concrete sign the `+0x70`/`+0x80` calls are doing
  genuine work.
- **But the value is string-shaped, not pointer-shaped** - e.g. one observed
  value, `0x2e35202028204d51`, decodes byte-by-byte (little-endian) to the
  ASCII fragment `"QM ( 5."`, plausibly part of a longer version string. The
  buffer was confirmed zeroed immediately before the `0xa0fc0` dispatch call,
  so this isn't stale/reused heap garbage - something in the `+0x70`/`+0x80`
  chain genuinely writes this.
- **A live SIGSEGV resulted**: a *different* real consumer than the
  previously-documented `0x180061000` bound-callback chain (found live at
  RVA `~0x50251`-`0x50264`, reached directly from `ctx->field_0x100` itself)
  unconditionally dereferences `field_0x8` as a real pointer (`mov rax,[rax]`
  twice, then `+0x28`, then a byte compare) with no type/mode check
  beforehand - crashes immediately on the ASCII data.

**This is a genuine struct-layout disagreement** (this offset apparently
means different things to different real consumers, or this project's
constructed object doesn't fully match whichever exact sub-variant the real
driver would normally allocate here), not a mistake in how this project
calls the real functions - the `+0x70`/`+0x80` calls are real, live,
non-crashing, WPP-traced code executing exactly as intended; their *output*
just isn't a valid pointer where this other consumer needs one to be.

**Fix applied to stay crash-free**: `field_0x8` is now unconditionally
overwritten with this project's existing quine fallback *after* the real
factory chain runs (previously only when still null) - the real `+0x248`
sub-object and the rest of the outer factory's initialization work are kept
(a genuine improvement over the old direct-`0xa0fc0`-call code), while the
one real field that's shaped wrong for its consumer is suppressed again.
Live-reconfirmed: back to the familiar crash-free `0xc00000b5` graceful
discard.

### Disassembled `+0x70` directly - it's a version-string formatter, and it also fixes the OTHER long-standing missing field (`+0x240`) as a side effect

Live-read the `+0x70` function pointer directly off the constructed object
and disassembled its body (`x/40i` plus a full range disassembly). Findings:

- It checks `self->field_0x240` (the *second* long-documented missing field,
  see "Found the real construction path for ctx->field_0x100" above -
  "these fields are architecturally meant to be filled in as a side effect
  of a real USB transaction completing... never... reachable this session")
  and, if null, **allocates a real `0x88`-byte buffer and stores it there
  itself** (`mov %rcx,0x240(%rax)`, live-confirmed) - no USB transaction
  needed after all; it just needed this real constructor path to actually
  run, same as `+0x248` before it.
- The rest of the function reads four individual bytes out of that new
  `+0x240` object, indexes a `rip`-relative table of 8-byte entries with one
  of them, and calls a `%64`-byte-limited formatting-shaped function
  (`edx=0x64`, a format-string-looking `r8` argument) - this is almost
  certainly building a human-readable version string (matching the `"QM ( 5."`
  fragment seen earlier) into a 58-byte local stack buffer, not writing
  directly into `self->field_0x8` - the earlier live value at `field_0x8` most
  likely aliased or was read from a stack-adjacent location connected to
  this formatting work, not a deliberate field write; still unconfirmed
  precisely which, but now understood to be incidental to this function's
  real purpose (version reporting) rather than a deliberate `+0x8` payload.

**Added a `field_0x240` diagnostic log and reconfirmed live**: now
consistently a real, distinct, non-null heap pointer every construction
(`0x555555573000`, `0x555555573120`, ...). **This resolves the second half
of the "two more real, still-unpopulated fields" open item** - both `+0x248`
(resolved earlier) and now `+0x240` are genuinely, correctly populated by
calling the real driver's own full construction chain (bare constructor ->
`0xa0fc0` dispatcher -> this outer `0x9ede0` factory), leaving only `+0x8`
still suppressed (to the quine fallback) due to the unrelated
pointer/string-shape mismatch documented above. Still `0xc00000b5` overall -
whatever `field_0x240`'s own real consumer does with this newly-real data
doesn't by itself complete the capture - but this is genuine, meaningful
forward progress: a field this document spent a long time concluding
"requires a real USB transaction, none of which is ever reachable" turned
out to just need the right constructor call, exactly like `+0x248` before
it, and no such transaction was actually required.

### Next step

`field_0x8` is the one remaining known-wrong-shaped field feeding
`ctx->field_0x100`'s success path. Two ways to make further progress on it:
(1) find `field_0x240`'s own real consumer (now that it's genuinely
populated for the first time, something upstream in the driver may read it
differently than before - worth a fresh full trace of a real enroll attempt
to see if any *new* code paths fire that didn't before), or (2) keep
investigating `+0x8` specifically - e.g. checking whether a *different*,
larger scratch buffer for the `+0x70` call (rather than the bare 8 zeroed
bytes this project currently passes) changes what ends up at `field_0x8`,
since the real call sites elsewhere in the DLL always pass a freshly
zeroed, stack-local 8-byte scratch too, so this project's own arguments
aren't obviously wrong - the mismatch may be structural (a field meant to
carry different data depending on some accompanying discriminant) rather
than something fixable purely from the caller's side.

### Pinned down the crashing field_0x8 consumer precisely, but its own call chain resists further static tracing

Found the exact crashing function (real start RVA `0x501c0`, the
`[rsp+0x70]`/`+0x8`-dereferencing code lives at `~0x50251`-`0x50264`) and its
**one and only caller anywhere in the DLL** (RVA `0x38bb7`), which computes
its arguments from what looks like an array iteration (`base+index*0x10+0x8`
as one argument, a flag byte from `base+index+0x78` as another) alongside a
separate, fixed `[rsp+0xc8]` value passed as the object whose `+0x8` field
gets dereferenced. Live-breakpointed the caller (safe to do repeatedly with
the current quine-suppressed `field_0x8`, since the quine's self-reference
never crashes): the object address seen there does **not** match any of
`try_construct_real_field0x100_obj`'s own logged `obj` pointers, and the
real backtrace at that point resolves through two of this project's own
generic COM stubs (`com_stub_36`, `com_stub_32`) with otherwise-unresolved
(`?? ()`) frames above them - i.e. this is reached through real driver code
calling back through a COM interface this project hasn't specifically
identified yet, not a straightforward call from the already-mapped
`0x1800125dc` purpose-dispatch/poll loop. Untangling this further by static
address-chasing alone is unlikely to be efficient from here - the next
productive step is almost certainly a fresh live trace anchored on this
exact caller (`0x38bb7`) with full argument/backtrace dumping across a real
capture attempt, rather than continued cold disassembly reading.

### Tested field_0x240 being real with an actual physical finger touch - no change

Since `field_0x240` (unlike `field_0x8`) is left as its genuinely real value
now (only `field_0x8` is suppressed), and its own consumer was never
confirmed reached in the fingerless case, tested whether a real touch
exercises it differently. Widened the watchdog, waited for the live prompt,
got a real touch confirmed in-chat at the same wall-clock second, held for
the window. **Same result as every prior test**: `0xc00000b5` after the full
window, and `field_0x240`'s value present but no new log lines/behavior
compared to the fingerless case (diffed full `-vv` traces line-by-line,
normalized for addresses/timing - byte-identical except pointer values and
the warm-up capture's own elapsed-time log). Watchdog reverted to 5s.

**Net conclusion for this pass**: `field_0x240` being genuinely real, by
itself, doesn't change observable behavior with or without a finger -
whatever consumes it either isn't reached in this build at all, or its
presence/absence isn't what gates anything downstream. The still-suppressed
`field_0x8` (real value is string/version-shaped, not the pointer its one
known consumer wants) remains the most concrete specific gap left in this
particular object graph.

### field_0x8 is a fixed, instance-independent tag, not per-object data - the crash consumer is very likely looking at the wrong object type entirely

Confirmed the static string is real (found `"QM ( %2d.%d.%d.%d for %s )"`
verbatim in `.rdata` via a direct binary search) - not memory corruption.
Then added a diagnostic for this project's own `extra_param` scratch buffer
(the `+0x70` call's second argument) and re-ran: **`extra_param_scratch`
stays exactly `0x0` after the call, every time** - `+0x70` never writes
through it despite both real call sites (and this project's own call)
treating it as an out-param-shaped argument. Combined with `field_0x8`
being the *exact same* value (`0x2e35202028204d51`) across multiple
constructions with visibly different `field_0x240`/`field_0x248` addresses
each time, this rules out "per-instance formatted version string" - a real
version string would vary with the object's own data. **`field_0x8` for a
type-tag-`0xca` object is far more likely a fixed, hardcoded identifying
tag/category marker inherent to this object type, not instance data.**

This reframes the crash: the one known consumer (RVA `~0x50251`) dereferences
whatever's at `field_0x8` as a real pointer with no type check beforehand.
If `field_0x8` is *always* this fixed tag for a genuine type-`0xca` object
(not a bug in how this project constructs it), then the crash consumer is
most likely operating on a **different kind of object than what this
project's `ctx->field_0x100` actually is** - consistent with the earlier,
unresolved observation that the crash caller's own object pointer
(`[rsp+0xc8]` at RVA `0x38bb7`) never matched any address this project's own
`try_construct_real_field0x100_obj` logged. This would mean the actual bug
(if any) is elsewhere: something in this project's WDF/COM emulation may be
handing the driver a type-`0xca` object in a context where the driver's own
logic expects a structurally-different object, rather than `field_0x8`
itself being "wrong." Not yet confirmed which object `[rsp+0xc8]` is really
supposed to be - would need a wider live trace (backtrace all the way to a
WDF/COM entry point, not just the immediate caller) to identify.

**Status**: `field_0x8` remains suppressed to the quine fallback (safe,
crash-free). This is now a well-characterized but still-open question -
resolving it further needs either real symbols or substantially more live
tracing than a single session reasonably affords; documented here in full
detail specifically so a future pass doesn't have to re-derive any of this.

**Correction, same pass**: the `com_stub_36`/`com_stub_32` backtrace frames
reported above (from the live-breakpoint trace at the crash caller) turned
out to be a real instance of this document's own documented pitfall - naive
frame-pointer-less `bt` producing stray stack values that happen to resolve
to valid symbols. Checked a full, unrelated `-vv` run of the same
(crash-free) build for literal `slot #36`/`slot #32` log lines: **zero
hits, anywhere** - those COM stub slots are never actually invoked in a
real run, so the earlier "frames" were misread stack garbage, not a real
call chain through COM dispatch. Retracting that specific inference; the
crash consumer's true caller chain remains unidentified beyond the single
confirmed direct caller (RVA `0x38bb7`).

### Experiment: pointing field_0x8 at ctx->field_0x1b8's real object - the crash moved deeper, and revealed this is a multi-object array walk, not a field_0x100-specific consumer

Tried the cheapest available hypothesis: since `ctx->field_0x1b8` is now a
genuinely real object (via `CTX260_CTOR_RVA`), what if `field_0x100->field_0x8`
is just meant to reference that same object rather than needing independent
construction? Wired `try_construct_real_field0x100_obj` to take the real
`field_0x1b8` object as a parameter and point `field_0x8` at it instead of
the quine fallback.

**Result: a genuinely different, deeper crash** - same crash function
(`0x501c0`), but now two real pointer dereferences succeed
(`field_0x8` -> a real pointer -> a real pointer) before failing on the
third (`+0x28` then a byte read). Live-inspected the full chain across
multiple hits of the crash-site breakpoint and found something important:
**the `self` object at this breakpoint never matches this project's own
constructed `ctx->field_0x100` object address, in *any* of several
different hits, across either the quine or the field_0x1b8 experiment** -
and one hit's chain resolved to a real, meaningful data structure
containing this exact device's own USB VID/PID (`0x0106cb0082`, i.e.
`06cb:0082`) at a plausible offset, while another hit's chain resolved into
what looks like raw x86 code bytes, not a data structure at all.

**This means the crash function at `0x501c0` is not specifically a
`ctx->field_0x100` consumer at all** - its one real caller (RVA `0x38bb7`)
walks an array of *several different* driver-internal objects (matching the
`base+index*0x10+0x8`-style addressing seen in that caller's own code),
checking each one's type/validity, and this project's `ctx->field_0x100`
construction may only be *indirectly* connected to this array (e.g. by
being registered into it somewhere this project hasn't found), if at all.
The specific entry that leads into invalid/code-shaped memory is the real
proximate cause of the crash, not `field_0x8` in isolation - `field_0x8`
being real just changes how far a *different* entry's chain gets before
hitting the actual bad one.

**Reverted the field_0x1b8 experiment** (kept the parameter threading in
`try_construct_real_field0x100_obj` for a future pass, but restored the
unconditional quine assignment) - not a fix, and the real array-walk this
crash function serves needs its own separate investigation (starting from
its caller's own caller, to find what populates this array and where this
project's own constructed objects would need to be registered into it, if
at all) rather than continuing to iterate on `field_0x8` specifically.
Reconfirmed crash-free after reverting.

### Assessment after this pass

This object-graph investigation (`ctx->field_0x100`, `field_0x8`,
`field_0x240`) has now yielded two genuine, real fixes (`field_0x240`, the
outer factory's `+0x70`/`+0x80` initialization) and one precisely-mapped but
still-open dead end (`field_0x8`, now understood to likely belong to a
different, unidentified array/registry rather than being a simple missing
field). Further progress on `field_0x8` specifically needs the array's own
origin found first - a materially different, larger investigation than
continuing to poke at this one consumer. Given real, coordinated finger
touches have shown zero behavioral difference from any fix in this whole
section, this object graph may not even be gating capture completion at
all - the `0xc00000b5` watchdog timeout's true cause remains, as it has
throughout this document, the `StartCapture` worker thread's own
`WaitForSingleObject` never being signaled with genuine completion by
anything this project's bridge currently supplies.

**Confirmed this directly**: live-breakpointed all 11 known callers of the
array-walk function (RVA `0x38800`, containing the crash site) across a full
real capture attempt. **Only the two earliest (lowest-RVA) callers ever
fire** (RVA `0x11afa`, `0x13756`, two hits each) - both well before
`STARTCAPTURE_THREAD_RVA` (`0x1c21c`) in address space and, live-confirmed
by hit timing, both fire during device *open*/`OnPrepareHardware`-era
initialization, not during the `StartCapture` devctrl or its worker thread
at all. **This closes the loop cleanly**: the `field_0x8`/array-walk crash
this section spent so long on is real, but genuinely unrelated to the
`StartCapture` completion blocker - it's an early-init-time code path this
project's `ctx->field_0x100` fixes happen to also touch, not something
gating capture itself. Effort on `ctx->field_0x100`/`field_0x8` specifically
should stop here; the real blocker remains exactly where the rest of this
document already, repeatedly, concludes it is - the worker thread's
`WaitForSingleObject`, signaled by nothing this bridge currently supplies.

## Live-traced every WaitForSingleObject/SetEvent in the whole process - confirms the core finding precisely, but corrects an outdated one: the worker thread now genuinely terminates

Per direct instruction to keep using live tracing rather than reasoning from
static disassembly, breakpointed this project's own `WaitForSingleObject`,
`SetEvent`, and the internal `win_set_event` (called directly by
`winio_complete_overlapped` for overlapped I/O completion, bypassing the
`SetEvent` WINAPI wrapper entirely - a real, separate signal path this
document hadn't previously distinguished from the WINAPI-level one) across
one complete, real enroll attempt (fingerless - real touch already
independently confirmed to make no difference to this specific mechanism
earlier in this document).

**Identified the exact StartCapture poll-wait live, unambiguously**, by
correlating each `WaitForSingleObject` call's direct caller address (via
`bt`, RVA-computed) against the already-documented `0x14e41`: two real
occurrences, `caller_rva=0x14e4b` (matching to 10 bytes - the small offset
is just measurement point, same call site), one per capture attempt
(warm-up + real enroll), each with its own freshly-allocated handle
(`0x7fffe0000c00`, `0x7fffe0044350`).

**Confirmed precisely, for both capture attempts**: `SetEvent` on that exact
handle *does* happen, but only *after* this project's own devctrl watchdog
has already force-completed the request (`winio_complete_overlapped`
calling `win_set_event` internally first) and the CLI has already logged
`Error capturing sample`/`Discarding enrollment` - i.e. this is
conclusively the driver's own **abandon/cleanup** signal, not a genuine
completion, exactly as prior sections of this document already concluded
from less direct evidence. This is now confirmed with hard, precise,
call-site-correlated live data rather than inference.

**New correction to prior documentation**: the earlier "Live-confirmed...
`ctx->field_0x198`... never clears, for the full 5-second observation
window... worker thread does not proceed to actually terminate" finding no
longer holds. Live-observed this session: right after the abandon `SetEvent`
fires, the worker thread's own GDB thread entry cleanly **exits**
(`[Thread ... (LWP ...) exited]`), and the corresponding "thread-join" wait
(RVA `~0x28788`, matching the previously-documented second wait site on
`ctx->field_0x198`) resolves without hanging for every worker thread spawned
this run (3 occurrences, none blocked past process completion). Checked this
project's own `CreateThread`/thread-join implementation (`winapi/thread.c`)
directly: it's a **correct, real implementation** - `thread_wait` uses
`pthread_join`/`pthread_timedjoin_np` under the hood, which only returns
once the underlying pthread's own start routine genuinely returns. This
isn't a bug in this project's shim; it's evidence that **the driver's own
worker-thread routine now actually reaches a real `return` statement** after
being abandon-signaled, something it apparently could not do reliably
before this session's accumulated fixes (real `ctx->field_0x1b8`,
`field_0x240`, the layout-mismatch/tcache fixes, etc.) - a genuine, positive
behavioral change, even though it's the abandon path completing cleanly,
not a success path.

**Net effect**: the core finding stands, now on firmer, live-verified
ground - nothing ever signals the capture-completion wait with genuine
success semantics before this project's own watchdog fires; only the
already-known abandon path ever touches it. But the process-level picture
is healthier than previously documented: worker threads reliably terminate
now, nothing hangs, and every abandoned attempt cleans up correctly. The
remaining gap is unchanged in kind - something needs to call `SetEvent` on
that handle, or otherwise complete the driver's own `WdfRequestComplete`
for the `0x440014` request, before this project's watchdog does it instead
- and nothing this bridge currently supplies (real WinUsb transfers during
`OnPrepareHardware` included) ever does that within the window.

## The real driver now initializes the StartCapture context by itself - the old ctx patches are now actively harmful

Logged the device context at the StartCapture `CreateThread` hook *before*
`patch_device_context` runs (new `DIAG: pre-patch` log line in
`driver.c: tudor_hook_thread_create`). On the very first capture of a session,
with no patch applied yet, the driver has already populated everything the
patches were written to fake:

    +0x100=<real obj> +0x1b8=<real obj> +0x144=1 +0x148=1 +0x137=130 +0x139=1739

So since `RetrieveDeviceName`/`IWDFPropertyStoreFactory` made
`OnPrepareHardware` do its real work, the device context is genuinely set up,
and `patch_device_context` *overwrites* real state (it replaces `+0x1b8` and
`+0x100` with newly-constructed objects and clobbers `+0x137`/`+0x139`, which look
like real sensor parameters, with `1`). `tudor_reset_ctx100` likewise throws
away the driver's real `+0x100` object, so the next StartCapture returns
`0x80070070`.

New env switches for testing (all off by default, so the default behavior is unchanged):
- `TUDOR_NO_CTX_PATCH=1` - skip `patch_device_context` and `tudor_reset_ctx100`
- `TUDOR_NO_WARMUP=1` - skip the warm-up StartCapture in `tudor_open`
- `TUDOR_WATCHDOG_SEC=N` - devctrl watchdog timeout (default 5)

With `TUDOR_NO_CTX_PATCH=1 TUDOR_NO_WARMUP=1`, a fingerless enroll runs with
no crash, and **for the first time the StartCapture itself issues real USB I/O**:
125-byte cmd `39 20 bf 02 ...`, cmd `1a`, a 10501-byte and an 18869-byte
write to EP 0x01 (encrypted/config blobs), a 2154-byte response on 0x81, then a
5-byte read on interrupt EP 0x83 (`00 00 00 00 00`). After that, the worker
parks in the known `WaitForSingleObject` (RVA `0x14e4b`) until the watchdog
fires. The discard path (`0x44201c`) also does real I/O (`04`, `52`, `39 01 ...`).

Also checked this session: the driver does all its WinUsb transfers
synchronously (`Overlapped == NULL`), so ignored overlapped completion is
*not* the missing signal. `winusb.c` now implements real async overlapped
transfers anyway (libusb async API + hEvent signaling, `AbortPipe`
cancellation, `PIPE_TRANSFER_TIMEOUT` honored, `WinUsb_GetOverlappedResult`),
plus verbose hex dumps of transfer data. The driver sets
`PIPE_TRANSFER_TIMEOUT=2000ms` on 0x81/0x01 and `SHORT_PACKET_TERMINATE` (policy 0x1) on 0x01.

### The 0x4a11c/0x4a12a NOP patches were dropping the finger-event callback - the capture path is now complete

Traced the interrupt path live (gdb, single-stepping the callback chain):

- A long-lived pool thread (started at open, `CreateThread` RVA `0xff02c`)
  runs the **interrupt listener loop** `0xf5610`: wait on `obj+0x28` → if
  `obj+0x58 == 1`, read 7 bytes from pipe index 3 (EP 0x83) via `0xf52d0` →
  `0xf4ac0` (`QueryPipe` + **synchronous** `ReadPipe`, `Overlapped=NULL`) →
  take the armed one-shot callback `{cb=obj+0x48, ctx=obj+0x50}` and disarm it →
  `cb(&{mode, status, len, buf}, ctx)`. Arming is `0xf54e0` (sets the
  callback, `+0x5c=1`, signals `+0x28`); disarm/abort is `0xf5570` →
  `0xf4dc0` (`WinUsb_AbortPipe(0x83)`).
- The callback chain is `0xf2920` (copies the buffer) → `0x66210` → `0x4a270`,
  which calls `slot->[0x8](mode, status, 0, slot->[0x10])`, where
  `slot = *(ctx->field_0x100)` (`= field_0x100 + 0x30`). **`slot->[0x8]` was
  NULL, so the event was silently dropped.**
- `slot->[0x8]`/`[0x10]` are written by `0x4a0a0(obj, mode, cb, ctx)`, i.e.
  exactly the two instructions `g_driver_patches` NOPs out (`0x4a11c`,
  `0x4a12a`). The worker calls it at `0x14e1d`, right before its
  `WaitForSingleObject` at `0x14e41`. Those NOPs were only ever needed
  because `field_0x100` used to be a fake object whose first qword was a
  tag, not a pointer. With the driver's own object it is a valid pointer.
- With the NOPs skipped (now done automatically under `TUDOR_NO_CTX_PATCH`):
  the callback `0x1127c` calls `SetEvent` on the worker's event
  (`0x7fffe0000c00`), the worker wakes, runs `0x46000` → `0x61000` →
  `0xea9d0` (parses the 5-byte interrupt payload), sees all zeros (no
  finger), re-registers via `0x4a0a0`, re-arms the listener, and waits
  again. **This is a correct "waiting for finger" loop.**
- The listener's second 0x83 read then blocks in the sensor until a real
  interrupt. It previously had a 15s libusb timeout and couldn't be aborted.
  `winusb.c` now runs *all* transfers through libusb async (sync callers wait
  on a condvar), so `WinUsb_AbortPipe` cancels them and timeouts follow the
  `PIPE_TRANSFER_TIMEOUT` policy (0 = infinite, as on Windows). Verified: on
  discard, `AbortPipe(0x83)` cancels the pending read (libusb status 3).

So under `TUDOR_NO_CTX_PATCH=1 TUDOR_NO_WARMUP=1` **none of the old
ctx/field_0x100/NOP workarounds are in play**, and the capture path is the
driver's own, end to end. The remaining question is whether a real touch
produces a `0x12`-type interrupt and a completed capture.

### FIRST SUCCESSFUL REAL CAPTURE - then a TIB bug in our own helper threads

Real finger test with `TUDOR_NO_CTX_PATCH=1 TUDOR_NO_WARMUP=1 TUDOR_WATCHDOG_SEC=60`:
the 0x83 listener received real finger events (`00 00 00 00 00` idle, then
`02 00 40 10 00`, `03 40 01 00 00`, `03 41 03 00 40`, `03 43 04 00 41`), the
driver sent `51 00 20 00 00`, read an 8082-byte response on 0x81, and
**StartCapture (`0x440014`) completed with status 0 and a 0x1f7-byte
result.** The CLI then crashed with SIGSEGV in "Updating enrollment"
(devctrl `0x442010`), on a fresh thread, at RVA `0xfd737`, inside `__chkstk`
(`mov byte [r11], 0` probing down from `gs:0x10` = `NT_TIB.StackLimit`).

Cause: threads that libtudor itself creates with plain `pthread_create` (the
overlapped-callback thread in `winapi/io.c` and the devctrl watchdog in
`tudor/device.c`) never called `win_init_tib()`. A Linux thread inherits its
creator's GS base, so driver code running on those threads used the
*creator's* TIB. `__chkstk` then probed from the creator's stack limit into
unmapped memory. Fixed by calling `win_init_tib()` at the start of both thread
functions.

### Enrollment loop works with real touches; the working path is now the default

With the TIB fix, a real enroll ran 15 captures. Every `StartCapture`
completed with status 0, and every `0x442010` update returned the 44-byte
progress struct, whose dword at offset 0x24 counts up as a percentage (8, 16,
25, 33, 41, 50, 58, 66). One capture was rejected (`0x80098008`, reject detail
5 = bad capture) and the retry worked. The test only ended because no touch
came within the then-60s watchdog at 66%.

Defaults changed accordingly:
- `patch_device_context`, `tudor_reset_ctx100`, the `0x4a11c`/`0x4a12a` NOPs
  and the warm-up capture now only apply under `TUDOR_LEGACY_CTX_PATCH=1`
  (kept for comparison only; `TUDOR_NO_CTX_PATCH`/`TUDOR_NO_WARMUP` are gone).
  The BUSY->READY status patch (`0x19778`) is still applied unconditionally.
  It hasn't been re-tested without it.
- The devctrl watchdog is now opt-in (`TUDOR_WATCHDOG_SEC=N`). A real capture
  waits for a finger with no timeout, exactly like on Windows.

Cancellation: the driver calls `IWDFIoRequest::MarkCancelable(IRequestCallbackCancel*)`
(slot 19) on its StartCapture request and `UnmarkCancelable` (slot 20) on
completion. Both were unimplemented stubs, so cancelling never reached the
driver. They are now bridged onto `WdfRequestMarkCancelable` (`com_activate.c`).
Verified: cancel → driver `OnCancel` → `AbortPipe(0x83)` → request completes
with `0x800703E3` → enrollment discarded cleanly. The CLI now cancels a pending
operation on Ctrl+C (`cli_wait_async`); its SIGINT handler previously set
`abort_cmd_loop = false` (an upstream typo), so it never aborted anything.

### Full enroll succeeded; identify hung -> four more real shim bugs fixed

A full real enrollment completed: progress went up to 100%, `0x442050`
exported the template (0xcc80 bytes), and the CLI printed "Successfully
enrolled finger". Identify then got as far as the `03 40 01 00 00` finger
event and stopped, with the capture worker at **100% CPU**. Fixed:

1. **`evt_wait` / `thread_wait` timed waits were completely broken (upstream
   bug).** They passed a *relative* `{timeout/1000 s, timeout*10^7 ns}` to
   `pthread_cond_timedwait`/`pthread_timedjoin_np`, which take an *absolute*
   `CLOCK_REALTIME` deadline. `tv_nsec` was also out of range, so the call
   returned `EINVAL`, which `cant_fail` doesn't catch because it only checks
   `< 0`. Result: any `WaitForSingleObject` with a finite timeout busy-looped
   forever, and a timed thread join returned "joined" immediately. The
   identify path waits on `field_0x100+0x30` with 2000ms (`0x4a204`, via
   `0xd93e0`). Now a proper absolute deadline is computed once
   (`win_timeout_to_abstime`).
2. **Teardown never ran the driver's PnP exit callbacks.** Added
   `tudor_com_remove_device`, which calls `IPnpCallback::OnD0Exit(D3Final)` and
   `IPnpCallbackHardware::OnReleaseHardware` before `winwdf_remove_device`.
   Without them, a cancelled capture's worker kept doing USB I/O after the
   DLL was unloaded (crash, "HeapAlloc called with invalid heap handle").
   The driver's teardown needed: cross-thread `PostThreadMessageW` (a
   thread-id -> queue registry in `window.c`, used to stop its power-window
   thread), `FindWindowW` (window list), `UnregisterClassW`,
   `(Un)RegisterPowerSettingNotification`, `WTSUnRegisterSessionNotification`,
   plus `TerminateThread` (warn-only), `CancelIo`, `GetCurrentThread`,
   `OutputDebugStringA/W`.
3. **`CreateFileA/W` returned the raw `libusb_device_handle*` as a HANDLE.**
   The driver's `OnReleaseHardware` `CloseHandle`s it, so `winhandle_destroy`
   freed/corrupted libusb's own handle, and `libusb_exit` then aborted
   (`usbi_mutex_destroy` assertion). It now returns a real winhandle with no
   destructor.
4. **Time APIs had wrong units (upstream bugs).** `QueryPerformanceCounter`
   computed `sec*10^6 + nsec`; it's now nanoseconds, with a matching
   `QueryPerformanceFrequency` (1e9). `GetTickCount` returned that raw
   counter; it's now milliseconds. `GetSystemTimeAsFileTime` computed
   `sec*10^4 + usec`; it's now real FILETIME (100ns ticks since 1601).

Verified: plain open/close and identify -> Ctrl+C -> close both exit normally.

### libfprint / fprintd path works

After installing the current build, `sudo fprintd-enroll` completed and
`fprintd-verify` returned `verify-match`, so the full fprintd → libfprint-tod
→ tudor-host-launcher → sandboxed tudor_host path works with the new shim (the
seccomp allowlist already covered everything the new code needs). The TOD
module hard-coded 8 enroll stages and reported every extra "more samples"
response as `enroll-retry-scan`. This driver's progress steps by 1/12, so
`TUDOR_NUM_ENROLL_STAGES` is now 12.

Notes: enrolling needs polkit authorization (`net.reactivated.fprint.device.enroll`),
so without a polkit agent (e.g. bare Hyprland) use `sudo fprintd-enroll -f
<finger> <user>`. Running `sudo ninja install` leaves root-owned files in
`build/`; `chown` it back afterwards.
