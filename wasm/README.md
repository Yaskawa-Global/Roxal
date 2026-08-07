# Roxal VM → WebAssembly

Originally a spike to determine whether the Roxal VM (~200k LOC C++, ANTLR4 → bytecode → interpreter)
can run in a browser, since that gates the whole web-IDE plan.

**Status: works.** Roxal executes `.rox` scripts in a browser tab and under node,
and passes its own test suite in wasm. Requires the pthreads build.

The DOM/web-UI work built on top of this — letting Roxal drive a browser
interface — is planned in `../web-integration-plan.md`.

## Result

| Stage | Result |
|---|---|
| ANTLR4 4.13.1 C++ runtime → wasm | ✅ clean, zero source changes |
| Roxal core (`libroxal.a`) → wasm | ✅ after 5 small fixes (below) |
| Link → `roxal.js` + `roxal.wasm` | ✅ 3.5 MB |
| Run a script, single-threaded | ❌ needs threads at VM construction |
| Run a script, `-pthread` | ✅ |
| Roxal's own test suite, in wasm | ✅ **523/540** — no confirmed wasm defects |
| Browser tab (COOP/COEP) | ✅ see `serve.py` + `index.html` |
| VM on a Worker, main thread free | ✅ `-sPROXY_TO_PTHREAD`; see `test-host.cjs` |
| Drive the DOM from Roxal | ✅ `dom` module; see `test-dom.cjs` and `index.html` |
| Publish Roxal state to a UI | ✅ `web` module; see `test-web.cjs` and `store-demo.html` |

No JIT and a feature-flagged build made the port far less painful than the line
count suggests.

## Why pthreads is required

Single-threaded wasm has no `std::thread`, and Roxal creates two OS threads
before any script runs:

1. **GC reclaimer** — `SimpleMarkSweepGC::startCollectorThread()`. Its comment
   states the thread exists in *every* build profile because it is the sole
   drainer of refcount-dead objects; `ROXAL_GC_DEDICATED_THREAD` only controls
   whether *tracing* also happens there. The existing "threadless" (VxWorks)
   profile is therefore not actually threadless.
2. **Dataflow engine actor** — `VM.cpp:1343`, `dataflowEngineThread->act(...)`.
   The signals/dataflow engine runs as an actor on its own thread.

Plus actors are a language feature, so scripts can create more.

Emscripten maps `pthread_create` onto **Web Workers** sharing one
`SharedArrayBuffer`. That requires the document to be **cross-origin isolated**
(`COOP: same-origin` + `COEP: require-corp`) — see `serve.py`. In production
those are set on the CloudFront behaviour for the IDE route; isolation is
per-document, so other routes can still embed third-party content and use
popup OAuth.

Costs to keep in mind: one Web Worker per pthread (megabytes each, so an
actor-heavy harness may eventually want an M:N scheduler multiplexing Roxal
actors onto a small pool), and memory growth is more expensive with shared
memory.

The alternative — driving the reclaimer and dataflow engine cooperatively from
`runFor()` turn boundaries — remains the route to a single-threaded build, but
it is deep surgery on core subsystems and risks native/wasm semantic divergence.
Not needed now.

## Portability fixes this required

These landed on `main` as "Misc dependency and compilation fixes for clang & wasm".

None are wasm-conditional; all are ordinary portability or correctness fixes.
Two are genuine latent bugs invisible on the native build:

- `core/atomic.h:822` — `atomic_stack::operator=` locked `m_lock` (a `std::mutex`)
  as `std::lock_guard<std::timed_mutex>`. The only `timed_mutex` in the file; a
  typo in a class-template member that is never instantiated natively. clang's
  two-phase lookup catches it because `m_lock` is non-dependent; GCC does not.
- `compiler/SimpleMarkSweepGC.cpp` — the GC traced `AsyncIOManager` roots
  unconditionally, but `AsyncIOManager.cpp` is only compiled when
  `ROXAL_ENABLE_FILEIO=ON`. **Any** `FILEIO=OFF` build fails to link, on any
  platform. Now gated.

Portability:

- `compiler/CallFrame.h` (new) — `Thread.h` forward-declared `CallFrame`, then
  instantiated `std::vector<CallFrame>` and `reserve()`d it in the constructor,
  which requires a complete type. libstdc++ tolerates it; libc++ does not. `VM.h`
  includes `Thread.h`, so the definition moved to a shared header.
- `compiler/Value.{h,cpp}` — `hash()` had `static_assert(sizeof(size_t) >= sizeof(uint64_t))`,
  false on wasm32. Now XOR-folds the 64-bit NaN-boxed word into `size_t`. Folding
  rather than truncating matters: the type tag is at bits 46–49 and QNAN at
  50–62, so a plain truncation would make every value sharing a low-32 payload
  hash identically. `if constexpr` keeps 64-bit behaviour bit-identical, and a
  future wasm64 build reverts to it automatically.
- `compiler/SimpleMarkSweepGC.cpp` — narrowing `uintptr_t + uint64_t` → `uintptr_t`.
- `compiler/{ASTGenerator,RoxalCompiler}.cpp` — dropped `boost::algorithm`; the
  only use was `replace_all` at two sites plus one unused include. Replaced with
  a new `roxal::replaceAll` in `core/common.{h,cpp}`. Avoids cross-compiling
  Boost. (`roxal::replace` already existed but substitutes only the *first*
  occurrence.)

Native suite after these changes: 864 pass, 0 unexpected failures, in both
conservative and precise-only GC modes — unchanged from baseline.

`compiler/roxal.cpp` still uses `boost::program_options`; the wasm host links
`libroxal.a` with its own entry point instead.

## Where the VM runs: the main thread is off limits

The host is linked `-sPROXY_TO_PTHREAD`, so `main()` — and therefore the VM —
runs on a Worker, leaving the browser's main thread free to service the event
loop. This is not a tuning choice. Roxal scripts block by nature (`sleep`, waits,
actor sends), and a blocking script on the browser main thread freezes the tab.

Two consequences the host enforces:

- `roxal_run_source` **refuses to run on the browser main thread**. Under node
  that path would appear to work while hiding the bug; in a browser it hangs the
  page. It would also latch `VM::markMainThread()` onto the wrong thread.
- Scripts are submitted, not called: `roxal_submit_source()` queues work for the
  VM thread and returns immediately, and callers poll `roxal_completed_count()`.
  The asymmetry is permanent — the browser main thread cannot `Atomics.wait`, so
  it must never wait on the VM. See `../web-integration-plan.md`.

`main()` with no argv serves that queue instead of exiting, so one VM outlives
many scripts. With an argv it runs that one file and exits (the CLI-shaped path
the test runner uses).

`wasm/test-host.cjs` pins all of this down.

## Layout

```
CMakeLists.txt  host target; a child of the root project, so ABI defines,
                include dirs, ANTLR4 and dataflow arrive by linking roxalcore
main.cpp        wasm host: submit queue + roxal_run_file / roxal_run_source
build.sh        convenience wrapper around emcmake configure + build
index.html      browser harness for the dom module (page driven from Roxal)
store-demo.html browser demo of the web module, rendered with NO framework
serve.py        dev server that sets COOP/COEP
roxal-bridge.js main-thread half of the JS bridge (--pre-js): handle table,
                op interpreter, DOM event -> Roxal callback queueing
test-host.cjs   host-level tests: threading model, inbound queue
test-dom.cjs    bridge tests for the dom module, against a DOM stub under node
test-web.cjs    state-bridge tests for the web module (snapshots, coalescing)
run-tests.cjs   runs Roxal's tests/*.rox through the wasm build under node
```

Output lands in `build-wasm-mt/dist/`.

## Reproducing

```bash
./install-deps.sh wasm        # emsdk + ANTLR4 C++ runtime cross-built for wasm
./wasm/build.sh               # configure + build -> build-wasm-mt/dist/

cd build-wasm-mt/dist
node ../../wasm/test-host.cjs   # threading model + inbound queue
node ../../wasm/test-dom.cjs    # dom module: marshalling, handles, events
node ../../wasm/test-web.cjs    # web module: store contract, coalescing
node ../../wasm/run-tests.cjs   # Roxal's own language suite
../../wasm/serve.py             # then open / or /store-demo.html
```

The build is ordinary CMake — `wasm/CMakeLists.txt` is added by the root
CMakeLists under `if(EMSCRIPTEN AND ROXAL_BUILD_WASM_HOST)`. `build.sh` only
spells out the configure flags; there is nothing in it to keep in sync.

Gotchas worth knowing:

- `-fwasm-exceptions` and `-pthread` must match across ANTLR4, the core and the
  host. Mismatches are ABI-level, not link errors. The root CMakeLists applies
  both to every `EMSCRIPTEN` build so they cannot drift; `install-deps.sh
  antlr4-wasm` passes the same pair.
- ABI feature defines used to be scraped out of `roxal_features.cmake` by the
  link script. They now arrive by linking `roxalcore` (which links `roxal_abi`
  PUBLIC) — one source of truth, no scraping.
- Builtin modules (`sys.rox`, `math.rox`) resolve relative to the working
  directory and load during `VM::instance()`, before `appendModulePaths()` can
  take effect — hence `chdir("/stdlib")`.
- Files written into the wasm FS from `preRun` land *before* `--preload-file`
  mounts `/stdlib`, so writing there that early aborts the module. Write after
  the module promise resolves.
- `MAXIMUM_MEMORY` defaults to 2 GB. With pthreads the maximum is declared up
  front on the shared memory, so raise it at link time if needed
  (`-sMAXIMUM_MEMORY=4294967296`) rather than expecting to grow into it.

## Test results: 525/540

112 tests are feature-gated out (fileio, socket, ffi, grpc, dds, regex, xml,
media, opencv, realsense, and the two ICU case-mapping tests), mirroring what
`runtests.py` skips for a build with those features off. 1 known-failing test is
excluded, as natively.

Of the 15 failures, none is a confirmed wasm defect. Two or three timing-dependent
tests come and go on top of that number under `JOBS=4` — `rt_execution`,
`gc_coordination_stress`, `signal_cleanup`, `actor_closure1` have all been seen —
and each passes when run on its own. The runner prints **every** failure, so the
count and the list always agree; if they ever disagree, believe neither.

| Failure | Cause |
|---|---|
| `typededucer_*` (7) | need `roxal --ast`; the wasm host has no AST-dump mode (`runtests.py:922`) |
| `stacktrace`, `exception_stacktrace` | traces embed the script path; native passes `../tests/x.rox`, the harness passes a bare name |
| `import_asset_sibling`, `import_folder_init`, `star_init_coexist` | import from package *directories* (`asset_pkg/`, `folder_init/`, `pkg1/`); the harness only writes flat `.rox` files into the wasm FS |
| `sys_paths` | asserts cwd is `/tests`; the harness chdirs to `/stdlib` |
| `repl_run` | REPL, which the host deliberately does not implement |
| `gc_scanner_selftest` | the *conservative* stack-scanner self-test. We run precise-only, and conservative scanning of wasm locals is exactly what cannot work — expected |
| `rt_execution` | timing-sensitive; passes when run serially, fails under `JOBS=4` |
| `gc_coordination_stress` | flaky natively too |

Closing the harness gaps (mirror the tests directory into the wasm FS including
subdirectories, pass a path-shaped script name, add an `--ast` entry point)
should take most of these to green.

## Known gaps

- `roxal --version` has no wasm equivalent, so `run-tests.cjs` reads the enabled
  feature set from `roxal_features.cmake` instead of self-reporting. Exporting a
  `roxal_features()` from the host would let tooling gate uniformly.
- fileio is off. Re-enabling it needs `AsyncIOManager` to run inline (its worker
  thread is now available under pthreads, but OPFS sync access handles are
  synchronous, so inline is still the better fit) and WASMFS+OPFS mounted.
