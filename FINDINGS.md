# 06cb:0082 (USB052) driver port - findings

Notes on getting Microsoft's `synaWudfBioUsb52.dll` / `synaBscAdapter52.dll`
(Synaptics WBDI driver, VID:PID 06cb:0082) running under libtudor. This is a
summary of how the driver actually works and what had to change in the
emulation layer. The day-by-day investigation log is in this file's git history.

**Status:** enroll, identify and verify work on real hardware, through both
the CLI and fprintd (libfprint-tod → tudor-host). Wrong fingers are rejected.
Enrollments persist across restarts. Cancellation, duplicate enrollments,
suspend/resume and clean shutdown all work. The driver DLLs run **unmodified**:
no binary patches, no faked driver state.

All RVAs below are relative to `synaWudfBioUsb52.dll`'s image base.

## How the driver works

### Activation and device setup

- The driver has no `FxDriverEntryUm` export. It is a legacy **UMDF 1.x COM
  driver**, activated via `DllGetClassObject` → `IDriverEntry`, and it talks
  to the framework through `IWDFDevice`/`IWDFIoRequest`/... COM interfaces
  instead of the flat WDF function table. `tudor/com_activate.c` bridges
  those COM interfaces onto libtudor's existing flat WDF implementation.
- Device add: `OnDeviceAdd` → `IWDFDevice::CreateDevice`, then (like a real
  UMDF host) `IPnpCallbackHardware::OnPrepareHardware` and
  `IPnpCallback::OnD0Entry`. Removal runs `OnD0Exit(WdfPowerDeviceD3Final)` and
  `OnReleaseHardware` (`tudor_com_remove_device`).
- `OnPrepareHardware` does all the real setup. It gets the device name from
  `IWDFDevice::RetrieveDeviceName`, converts it with `WideCharToMultiByte`, and
  wildcard-matches it against `"usb"` (no match = it silently does nothing).
  It reads device properties through `IWDFPropertyStoreFactory`, then opens the
  device itself with `CreateFileA` and drives it **directly through the WinUsb
  API** (`LoadLibraryA("WINUSB.DLL")` + `GetProcAddress`), not through WDF USB
  targets. During open it runs a pairing/TLS-shaped conversation with the
  sensor and uploads config blobs.
- With that working, the driver builds its own capture context (the
  `+0x100`/`+0x1b8`/... fields older versions of this port used to fake).

### USB endpoints

| EP | Type | Use |
|---|---|---|
| 0x01 | bulk out | commands / config blobs (`PIPE_TRANSFER_TIMEOUT` 2000 ms) |
| 0x81 | bulk in | command responses (2000 ms timeout) |
| 0x82 | bulk in | large data block (51224 bytes) during open |
| 0x83 | interrupt in | 5-byte sensor events (finger state) |

All transfers the driver issues are synchronous (`Overlapped == NULL`).

### Capture path

- **Interrupt listener:** a pool thread started at open (`CreateThread` RVA
  `0xff02c`) runs the loop at `0xf5610`. It waits for an "arm" request, reads 7
  bytes from EP 0x83 (`0xf52d0` → `0xf4ac0`: `QueryPipe` + a blocking `ReadPipe`),
  then hands the result to a one-shot callback. Arm = `0xf54e0`; abort = `0xf5570`
  → `WinUsb_AbortPipe(0x83)`.
- **StartCapture** (devctrl `0x440014`) spawns a worker (RVA `0x1c21c`). It sends
  the capture setup (125-byte command, config blobs), registers its
  finger-event callback (`0x4a0a0`, which writes the callback into
  `*(ctx->field_0x100)`), arms the listener, and waits on an event
  (`WaitForSingleObject` at `0x14e41`).
- **Events** arrive on EP 0x83 as `00 00 00 00 00` (idle), then `02 00 40 10 00`,
  `03 40 01 ..`, `03 41 03 ..`, `03 43 04 ..` as a finger lands. The callback
  chain (`0xf2920` → `0x66210` → `0x4a270` → `0x1127c`) signals the worker, which
  parses the event, re-arms, and loops until the finger is fully down. Then it
  sends `51 00 20 00 00` and reads the ~8 KB image/template response, and
  StartCapture completes with a 0x1f7-byte result.
- The request is marked cancelable (`IWDFIoRequest::MarkCancelable`). Cancelling
  runs the driver's `OnCancel`, which aborts EP 0x83, and the request completes
  with `0x800703E3`.
- The identify path also uses a 2000 ms timed wait (`0x4a204`).

### Devctrl codes seen

| Code | Purpose |
|---|---|
| `0x440008` | reset |
| `0x440010` | query status (out: 0x14 bytes, status at +0x8, `3` = ready) |
| `0x440014` | StartCapture (in: 0x20 bytes, purpose at +0x4) |
| `0x44200c` / `0x44201c` | begin / end (discard) enrollment |
| `0x442010` | update enrollment (out: 0x2c bytes; progress % at +0x24, steps of 1/12) |
| `0x442050` | export template (≈52 KB) |
| `0x442054` | load template for matching |
| `0x442004` / `0x442008` | identify / verify result |
| `0x442014` | duplicate check result |

Matching happens on the host ("Enroll on Host Match on Host").

## What had to change in the emulation layer

These are bugs or gaps in libtudor's WinAPI/WDF shim. Most of them are generic
and would affect other drivers too.

### COM bridge (`tudor/com_activate.c`)
- UMDF 1.x activation, a device object, I/O queues, and request facades
  (heap-allocated: async requests outlive `OnDeviceIoControl`).
- **Out-params that must be filled in:** generic stubs returned `S_OK` without
  writing out-params, and the driver then used uninitialized pointers.
  `RetrieveDeviceName` (must contain `usb`, or OnPrepareHardware does nothing),
  `RetrieveDeviceInstanceId`, `RetrieveDevicePropertyStore`, and
  `IWDFPropertyStoreFactory::RetrieveDevicePropertyStore` (the
  "PairingContext" lookups) are real implementations now.
- `GetInputMemory`/`GetOutputMemory` return a small scratch buffer when the
  request has none, because the driver reads them unconditionally.
- `MarkCancelable`/`UnmarkCancelable` are bridged onto
  `WdfRequestMarkCancelable` (needed for cancellation).
- Unimplemented slots are logged at debug level. The ones that fire
  (`StopIdle`, `ResumeIdle`, `GetSystemPowerAction`, `AssignS0IdleSettingsEx`,
  `CreateDeviceInterface`, ...) are PnP/power plumbing with no Linux
  equivalent, and `S_OK` is the right answer for them.

### WinUsb (`winapi/winusb.c`) / SetupAPI (`winapi/setupapi.c`)
- WinUsb on top of libusb. All transfers use libusb's async API, so
  `WinUsb_AbortPipe` can cancel even a blocking synchronous read (the listener's
  EP 0x83 read blocks until a finger event).
- `PIPE_TRANSFER_TIMEOUT` is honored (0 = infinite, like Windows). Overlapped
  transfers complete their `OVERLAPPED` and signal `hEvent`.
- `CreateFileA/W` accept the fake device paths and return a real winhandle
  (the driver `CloseHandle`s it on teardown).

### Threads, waits, time
- `WaitForSingleObject` with a finite timeout and timed thread joins passed a
  relative, out-of-range `timespec` where pthread expects an absolute deadline.
  The resulting `EINVAL` was swallowed, so timed waits busy-looped forever
  (identify hung at 100% CPU) and timed joins returned immediately.
- Threads created by libtudor itself (overlapped-callback thread) now set up
  their own TIB. A new pthread inherits its creator's GS base, so `__chkstk`
  probed against the wrong stack limits.
- `QueryPerformanceCounter` (now ns, plus `QueryPerformanceFrequency`),
  `GetTickCount` (now ms) and `GetSystemTimeAsFileTime` (now real FILETIME) had
  inconsistent units.
- `-mno-red-zone`: Windows-ABI callers don't respect the SysV red zone.

### Other WinAPI fixes
- `WideCharToMultiByte`/`MultiByteToWideChar` only converted one character.
- `BCryptEncrypt`/`BCryptDecrypt` didn't write back the chained CBC IV.
- `EnterCriticalSection` no longer writes `LockCount`/`RecursionCount`/
  `OwningThread` (that corrupted driver memory aliasing the struct).
- `GetModuleFileNameW` returns a full path. Stack-cookie pre-population and
  copying the PE headers into the image (the driver reads its own headers).
- New APIs: hidden window + message queue (`window.c`: cross-thread
  `PostThreadMessageW`, `FindWindowW`, `Get/SetWindowLongPtrW`, ...),
  power/session notification stubs, `VirtualProtect`, `HeapSize`, event-log
  and ETW stubs, `TerminateThread` (warn-only), and more.

### tudor-host / libfprint-tod / CLI
- tudor-host aborted when the engine flagged an enrollment as a duplicate
  (`tudor_enroll_commit` returns false for duplicates, so the "overwrite
  duplicate" branch was unreachable). It now reports `duplicate`/`failed` in the
  enroll response, and fprintd shows `enroll-duplicate`.
- libfprint-tod uses 12 enroll stages (this driver's progress step).
- The CLI's SIGINT handler set the abort flag to `false`. Ctrl+C now cancels a
  pending capture.

## Building, installing, testing

- Install with `sudo meson install -C build --no-rebuild` (or `ninja install`),
  not by copying build-tree binaries: `tudor_host`'s RUNPATH only gets
  rewritten to `/sbin/tudor` on install. The build directory must be
  configured by the same meson version root uses.
- fprintd enrollment needs polkit authorization. Without a polkit agent, use
  `sudo fprintd-enroll -f <finger> <user>`.
- Debug knobs: `TUDOR_LOGLVL=0` (tudor-host) / `-vv` (CLI) for verbose logs,
  plus `TUDOR_LOG_USB_DATA=1` to also hex-dump USB payloads (off by default so
  raw sensor traffic doesn't end up in the journal).

## Debugging notes

- Breakpoints on driver code: the DLL is mapped at runtime and libtudor has no
  debug symbols. Break on libtudor's `CreateThread` (ms_abi: the start address is
  in `r8`). The first thread the driver creates starts at RVA `0xff02c`, so
  `base = $r8 - 0xff02c`, then `break *(base + rva)`.
- Calls into driver code need `__winfnc` (ms_abi) function pointer types.
- gdb's backtraces through PE code are unreliable past the first frame (there's
  no unwind info). Check that a "return address" really follows a `call`.
- To trace a callback chain, single-step from Python at the top level (not from
  a breakpoint's `stop()`), with `set scheduler-locking step`.
- The passwordless sudo setup used during development only covered `gdb`, so the
  CLI was run as `sudo gdb -batch -x script --args ./build/cli/tudor_cli store.dat -vv`.

## Known gaps

- Only tested on one machine/sensor.
- `TerminateThread` can't actually kill a thread (it's only a fallback the
  driver uses during teardown).
- Shutdown now runs the driver's real teardown and takes a few seconds.
