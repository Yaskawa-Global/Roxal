# fileio: synchronous-by-default semantics and the wasm backend

Decisions settled 2026-08-08 (design discussion with David). This document
records what was decided and why, so the implementation and its follow-ups
stay anchored.

## The semantic change (M1 — done)

Every fileio operation that goes through the I/O worker (`read`, `read_line`,
`read_file`, `write`, `flush`, `close`) is **synchronous from the script's
point of view by default**, with an `async:bool=false` parameter to opt back
into the old future-returning behaviour.

The futures were originally added so RT-loop scripts could do I/O without
blocking `runFor()`. That concern is real but belongs in the VM, not the API:
`async=false` submits to the same worker queue and then **awaits the future
inside the VM dispatcher** (`sys.wait(for=)`'s machinery — pendingWaitFor +
waitSuspension). The Roxal thread parks; the OS thread does not. Under RT,
`runFor()` returns immediately while the I/O is in flight; under the web
host, the pump keeps running. `wait()` passes non-futures through unchanged,
so pre-M1 scripts (`wait(for=fileio.close(f))`) work without modification.

What this bought immediately:
- beginner-shaped code (`var text = fileio.read_file(p)`) that actually works;
- close()/flush() no longer hand back futures nobody awaits — the
  swallowed-error / abandoned-write history came straight from that;
- `async=true` remains for pipelining and fire-and-forget writes from an RT
  loop, consumed with `wait(for=...)`.

Semantics worth remembering:
- One worker, one FIFO queue → read-after-write ordering holds per handle in
  both modes. Cross-handle visibility (write via handle g, read via
  `read_file(path)`) still requires `flush(g)` — ordinary buffered-I/O
  behaviour, same as Python.
- Errors resolve to the op's failure VALUE (false / nil), same as the async
  path always did. The error-model question (values vs exceptions) is
  deliberately untouched.

### The VM bug this flushed out (fixed)

Natives invoked through the **deferred-default-parameter continuation path**
(`processNativeDefaultParamDispatch`) did not honor a wait-suspension set
during the call: the suspension dangled and hijacked the *next* native call's
result slot. Any `.rox`-declared builtin with a defaulted parameter that
tried to suspend would hit this — fileio's `async:bool=false` was simply the
first. Fixed by mirroring `callNativeFn`'s suspension capture at the
continuation path's result-store (init methods excluded — their result is the
receiver by construction). Related pre-existing wart, still open: builtin
behaviour differing between the defaulted and non-defaulted call paths is
also why builtin-throw catchability varies with defaulted args.

## The wasm backend (M2)

fileio's implementation is plain `std::fstream`; under Emscripten those calls
go through Emscripten's virtual FS. So the wasm "backend" is a **build/mount
decision, not new Roxal code**:

- **Tier 1 (this milestone)**: compile fileio into the wasm build
  (`-DROXAL_ENABLE_FILEIO=ON`, was OFF). Files live in MEMFS — fully
  functional, ephemeral (lost on reload). The AsyncIOManager worker is one
  more pthread; PTHREAD_POOL_SIZE may need a bump if boot hangs appear.
- **Tier 2 (evaluate)**: switch to WASMFS with its OPFS backend for
  persistence. WASMFS implements exactly the plumbing we would otherwise
  write by hand (sync access handles, worker proxying, the async-open dance);
  if it holds up, the same fileio C++ works against persistent
  origin-private browser storage unchanged. Risks: WASMFS is a different FS
  implementation (the /stdlib staging must survive), and OPFS sync handles
  take an exclusive per-file lock. Node (the test runner) has no OPFS, so
  tests keep running against in-memory storage either way.

  Status: **shipped.** `ROXAL_WASM_WASMFS` cmake option (default OFF;
  wasm/build.sh turns it ON), full suite identical to MEMFS. The host mounts
  the OPFS backend at **/data** during startup (wasm/main.cpp,
  `wasmfs_create_opfs_backend`) when `navigator.storage` exists; under node
  or without WASMFS, /data is a plain directory at the same path, so scripts
  never care which they got. The IDE keeps its files there via the web
  module's Workspace actor (fs_list/read/write/delete over fileio itself) --
  verified persisting across a browser reload.

  WASMFS caveat found the hard way: the `--preload-file` mount (/stdlib, which
  is also the cwd) is effectively READ-ONLY -- creating a directory there
  traps (`unreachable`) and unlinking a file there fails, while plain file
  CREATION quietly works. Regular WASMFS directories (/tests, /data) behave
  fully. Tests therefore keep the house convention of working under
  ../tests/, and scripts should treat /data as the writable area. Also keep
  PTHREAD_POOL_SIZE at 8 -- a precautionary bump to 10 was harmless alone but
  worth avoiding.

OPFS facts that shaped this (corrections from the design discussion):
- OPFS is async everywhere, PLUS a sync fast path (`createSyncAccessHandle`)
  available only in dedicated workers — which is where our VM already lives.
- OPFS + FileSystemHandle are the WHATWG File System standard (all engines);
  the *pickers* are a WICG proposal, Chromium-only, with Mozilla/Apple
  opposed to write access.

## Dropped: the picker tier (was M3)

Decision: **no browser local-folder backend.** In-browser storage is OPFS;
desktop gets real host FS through Electron, where Node has direct access (VS
Code itself is the precedent) — via NODEFS/NODERAWFS mounts for a wasm VM or
by bundling the native roxal binary. Either way no picker, no new Roxal
backend. If the picker API ever standardizes across engines, only handle
acquisition differs from OPFS, so retrofitting is cheap; files can meanwhile
move in/out via `webkitdirectory` import and download export.
