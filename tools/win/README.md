# UIA harness

The full Windows product build needs additional dependencies. This harness
builds the real accessibility core and UIA backend independently.

In an MSYS2 CLANGARM64 shell with libcjson installed:

```sh
bash tools/win/build-uia-harness.sh
./build-win-uia/test_atspi.exe
./build-win-uia/test_uia_liveness.exe
```

The last command is the **session-0** cell: run it through Windows OpenSSH.
It requires an allocated backend with `is_connected == false` and tree access
rejected. A COM pointer or an empty desktop root is not proof of readiness.
The generic tests skip live cells unless `MD_A11Y_LIVE_TESTS=1` is set.

## Interactive desktop

The user must be logged in at the VM console. In that console (not SSH), with
`C:\msys64\clangarm64\bin` on PATH:

```powershell
.\build-win-uia\test_uia_liveness.exe --interactive
$env:MD_A11Y_LIVE_TESTS = '1'
.\build-win-uia\test_atspi.exe
.\build-win-uia\atspi_dump.exe --compact
```

From SSH, scp a PowerShell script rather than nesting shell quotations. Have
it register a uniquely named one-shot task using
`New-ScheduledTaskPrincipal -UserId (whoami) -LogonType Interactive`, run the
commands above, capture their individual exit codes, wait for completion with
a deadline, and stop/unregister the task in a `finally` block. Delete temporary
scripts and outputs when finished. SSH itself runs in session 0 even when
`quser` reports an active console.

## Real lock/disconnect qualification

The normal liveness test injects provider errors at the root-call boundary and
sends session messages through the real listener window. These do **not** prove
that Windows delivered an actual lock/disconnect notification.

For that separate, disruptive cell, coordinate console access first. Launch
`test_uia_liveness.exe --wait-for-loss` in the interactive console (or an
Interactive scheduled task), then lock the VM with Win+L or disconnect that
session within 30 seconds. The test waits on an event, asserts terminal loss,
then destroys the backend. Save its output/exit code, unlock the console, and
remove any scheduled task. Repeat separately for disconnect if required.

## Lifetime and COM coverage

Both session modes exercise root errors (`RPC_E_DISCONNECTED`,
`UIA_E_ELEMENTNOTAVAILABLE`, disconnected objects and unavailable RPC servers),
non-terminal resource failure, foreign-session filtering, terminal lock/logoff/
disconnect messages, a late callback after backend destruction, incompatible
STA rejection, and destruction while one root call is blocked and another tree
caller is admitted but queued. Synchronization uses events/semaphores, not
sleep-based ordering. Test seams exist only in the standalone test translation
unit, never in the production backend object.

The liveness predicate performs only an Interlocked read. WTS monitoring runs
from initialization until destruction, independent of change subscriptions.
Root-provider failures are observed when a root is accessed; no periodic COM
probe detects a silent provider failure while idle. Loss is terminal: unlock or
reconnect requires a new context. Owners must stop starting new API calls before
destroy; admitted tree calls and late callback dispatch are drained internally.

Also compile `src/core/a11y_uia.cpp` with MSVC ARM64 `/std:c++17 /W4 /WX /EHsc
/permissive-`. Use real dependency headers or the VM's documented type-only
pthread/cJSON stubs for this compile-only check. Do not define
`WIN32_LEAN_AND_MEAN` (it breaks UIAutomationCore headers).
