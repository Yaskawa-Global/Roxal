# Roxal on the web — DOM/JS integration plan

Turning the `wasm/` spike into a way to build browser UIs in Roxal, with an Electron
desktop story later.

Companion to `wasm/README.md` (which covers the port itself) and `qt-integration-plan.md`
(whose module is the structural precedent for everything here).

## Decisions taken

| | |
|---|---|
| Threading | VM on a worker (`PROXY_TO_PTHREAD`); DOM proxied to the main thread |
| Authoring | Roxal for state + logic, React/JSX for the view. **The state bridge is the product.** |
| Scope now | Layers 0–2 below. The Roxal widget toolkit (layer 3) is deferred. |
| 3D | Design the seam, build nothing yet |

Rejected: VM on the browser main thread driven by `runFor()` slices — direct
`emscripten::val` and no protocol, but any blocking Roxal idiom (`sleep`, a
blocking wait, a long loop) freezes the tab. Roxal scripts block by nature, so
that is a permanent footgun rather than a constraint users can be taught.

Rejected: Asyncify — size and speed cost, composes badly with pthreads.

## Architecture

```
browser main thread                        VM worker (PROXY_TO_PTHREAD)
──────────────────────                     ────────────────────────────
React / app JSX                            app.rox
  │                                          │
  ├── useSyncExternalStore ◀── snapshot ─────┤  ChangeNotifier per property
  │      (frozen, versioned)     (async,      │  (RoxalStore, layer 2)
  │                              coalesced)   │
  │                                           │
  └── app.set_target(21.5) ─── call ────────▶ │  HostEventLoop::pump() drains
         returns Promise         (async,      │  the inbound queue at a
                                  never sync)  │  dispatch-loop safepoint
                                               │
handle table                                   │
  [42] <div>          ◀──── op / op batch ──── │  dom module (layer 1)
  [43] CanvasCtx           (sync round trip     │
                            for reads, queued   │
                            for writes)         │
                                                │
OffscreenCanvas ──── transferControlToOffscreen ▶  future gpu module
                     (one-time handshake)          (webgpu.h, no proxying)

GC reclaimer thread   dataflow engine actor thread   (Roxal's own, DOM-blind)
```

Two rules make the whole thing safe, and both are load-bearing:

1. **Roxal → JS may be synchronous. JS → Roxal is *always* asynchronous.**
   The VM worker may block waiting on the main thread; the main thread must never
   block waiting on the VM. That eliminates the entire deadlock class by
   construction. JS-side calls return Promises, no exceptions.
2. **Inbound calls land at a safepoint, never mid-instruction.** Same
   run-to-completion discipline the Qt hub enforces (see the depth-counter fix in
   `QtSignalHub`); the actor model requires it.

### Why the worker choice makes React *easier*

Had the VM run on the main thread, inbound calls would re-enter the VM during
React's render phase and outbound state changes would land mid-render. On a
worker the boundary is already async and batched — which is exactly what
`useSyncExternalStore` wants. The two decisions reinforce each other.

## Layer 0 — the bridge

Everything else sits on this. `compiler/web/JsBridge.{h,cpp}` + a main-thread JS
half in `wasm/roxal-bridge.js`.

### Handle table (main thread)

JS values Roxal holds are kept alive in a main-thread array, referenced by
`int32` handle. Handle 0 is null. Freed slots are recycled through a free list.

### Marshalling

A compact tagged encoding written into a per-thread scratch buffer in the shared
heap. The main thread decodes straight out of `HEAPU8` — memory is shared, so
reads are zero-copy.

```
tag u8
 0 nil        3 int32      6 handle (u32)     9  callback (u32 id)
 1 false      4 float64    7 array  (u32 n)   10 bytes    (u32 len)  → Uint8Array view
 2 true       5 string     8 object (u32 n)
```

`bytes` matters more than it looks: it is how a tensor reaches `putImageData`
without a copy.

JSON was the alternative. Rejected — lossy for handles and callbacks, and it
gives up the zero-copy property that shared memory hands us for free.

### Ops

```
GET handle,name              → value        NEW    handle,args      → handle
SET handle,name,value                       INDEX  handle,i         → value
CALL handle,name,args        → value        SETIDX handle,i,value
GLOBAL name                  → handle       RELEASE handle
LISTEN handle,event,cbId     → listenerId   UNLISTEN listenerId
```

### Sync vs. queued — and the cost that drives it

A synchronous proxied call costs one main-thread event-loop turn. Chatty code
pays that per op, so the split is:

- **Queued, no round trip**: `SET`, `SETIDX`, `RELEASE`, `UNLISTEN`. They have no
  result, so there is nothing to wait for. They accumulate in a shared ring buffer.
- **Sync round trip**: everything else. Flushes the ring buffer first, so
  ordering is never violated.

Plus an explicit `dom.batch(proc(): …)` for mutation-heavy stretches.

This cost is why layer 2 is the product and layer 1 is the escape hatch: when
React owns the DOM, the hot path is one coalesced state snapshot per change
batch, not thousands of individual DOM ops. The round-trip problem largely
evaporates for the intended authoring style. Prior art for the general shape:
Google's worker-dom.

### Inbound: JS → Roxal

`VM::setHostEventLoop()` already exists for exactly this — the qt module installs
a `HostEventLoop` and the dispatch loop services it cooperatively
([compiler/VM.h:83](compiler/VM.h#L83)). A `WebHostLoop` is a drop-in:

- `pump()` — drain the inbound call queue, non-blocking.
- `waitForEvents(maxWait)` — block until a JS call arrives or the timeout fires.
  This is what an idle UI app does, so `web.serve()` is just the VM sitting in
  `waitForEvents`.

Each drained call is dispatched via `ActorInstance::queueCall()`, which already
returns a Future — resolve the JS-side Promise from it. If the exposed object is
a plain object rather than an actor, the bridge invokes it on the VM thread
directly at the same safepoint.

The hazard being avoided is the one the DDS lift already hit: touching VM state
from a non-VM thread corrupts it. The main thread only ever appends to a
lock-free queue; the VM thread does all the VM work.

**A UI app must park inside a script** — `dom.run()`, and later `web.serve()`.
This is not a stylistic choice, and it cost some debugging to establish:

- Handlers execute on the VM thread via `invokeClosure`, which needs a live Roxal
  `Thread`. Draining the queue *between* scripts fails with "Value stack overflow
  (limit 0)" — that thread has no stack, because no script is running.
- The park must not block in C++ either. A native builtin that loops waiting for
  events holds the VM inside a native frame, and a handler invoked from there
  re-enters the dispatch loop in a state it does not expect — an outright wasm
  trap, not a graceful error.

The working shape is qt's: `run()` sets `threadSleep` and **returns**. The
dispatch loop keeps running, services the host event loop, and invokes handlers
with a proper thread context (`ParkedInvokeScope` un-parks around each one).
`stop()` only records the request; the host loop performs the un-park *after* the
handler returns, because the scope would otherwise re-park on return.

Consequence for the API: a script that installs listeners and then ends does
nothing at all. That has to be loud in the docs, and it is the same contract qt
already has.

## Layer 1 — the `dom` module

Direct DOM/JS access from Roxal: the escape hatch, and the substrate anything
else needs. `ObjJsValue` is `ObjQtObject` with a handle instead of a `QPointer` —
`tryGetDynamicProperty` / `trySetDynamicProperty` / `tryInvokeDynamicMethod`, a
new `ObjType::JsValue`, `trace()` empty (no Roxal Values held).

```roxal
import dom
var el = dom.document.get_element_by_id("out")
el.text_content = "hello"
var ctx = dom.document.query("#c").call("getContext", ["2d"])
```

Name mapping: Roxal `snake_case` ↔ JS `camelCase` by default, with
`dom.get(el, "exact-name")` / `dom.set(...)` as the escape hatch for names that
are not valid Roxal identifiers — same convention the qt module already uses.

Two things the implementation settled that were not obvious up front:

- **Reading a JS function property must yield a bound callable, not a handle.**
  Roxal compiles `x.foo(a)` to `GetProp(x,"foo")` followed by a call, so the read
  has to produce something callable. `Tag::Method` carries receiver + name, and
  the receiver travels as a *fresh* handle owned by the binding, so the binding
  can neither outlive its receiver nor leak it.
- **Plain data crosses by value; objects with identity cross by reference.**
  Arrays and plain objects become Roxal lists and dicts; anything with a
  prototype of its own (DOM nodes, class instances, functions) becomes a handle.
  Getting this wrong is not subtle in either direction — an event payload
  arriving as an opaque handle cannot be indexed, and a DOM node arriving as a
  dict would be a dead copy. This matches the value/reference split the qt module
  found for QML.

## Layer 2 — the `web` module (the state bridge)

The product. A direct port of `RoxalPropertyMap` from QML to JS.

```roxal
import web

type App object:
  var temp :signal = signal(0)
  var target :real = 20

  proc set_target(v :real):
    self.target = v

web.expose("app", App())
web.serve()                  # block in HostEventLoop::waitForEvents
```

```jsx
const rox = await createRoxal({ script: '/app.rox' });

// adapter, ~50 lines
export function useRoxal(name) {
  const s = rox.store(name);
  return useSyncExternalStore(s.subscribe, s.getSnapshot);
}

function Panel() {
  const app = useRoxal('app');
  return <Gauge value={app.temp} onChange={v => app.set_target(v)} />;
}
```

C++ side (`RoxalStore`), mirroring `QtBindable.cpp` role for role:

- **Roles** — `orderedPublicProperties()` for stored `var`s, plus computed
  accessors discovered from their `__get_`/`__set_` methods. Identical to
  `RoxalPropertyMap::buildRoles()`.
- **Change → JS** — a `ChangeNotifier` per role marks it dirty; dirty roles are
  coalesced and posted as one delta.
- **Methods → JS** — public, non-overloaded methods become callable; each returns
  a Promise. Mirrors `buildMethods()` / `installMethods()`.
- **JS write → Roxal** — routed through gated `assign()` for stored properties and
  `__set_` for computed ones; get-only accessors are read-only. Mirrors
  `updateValue()`.

**Snapshot contract.** `getSnapshot()` must return a referentially stable value
or React tears. The main thread keeps a frozen snapshot object and replaces it
wholesale on delta, bumping a version. Notification is coalesced into one
microtask so a burst of Roxal writes produces one React render.

Framework-agnostic on purpose: the primitive is `subscribe` / `getSnapshot` /
`call`. Svelte's store contract *is* `subscribe`; Vue is a `shallowRef`. React is
one adapter among several, not a dependency of the runtime.

## Signals or events?

Both, and the split follows one criterion: **is dropping an intermediate a bug?**
Coalesce vs. queue.

| Direction | Channel | Why |
|---|---|---|
| Roxal state → view | **signal / observable property** | React renders a *current value*; an event has none. Coalescing is required, not merely tolerable. |
| Roxal → view, one-off | **event** | "Toast", "job finished", "navigate". A signal cannot express *notify twice with the same payload* — no change, no notification. |
| View → Roxal | **method call** | An occurrence that needs a reply. |

Signals matter because a signal-valued property changes *inside* the signal —
the property slot never changes — so the store has to observe the signal itself.

The rate claim holds, but only for the idiomatic form, and it took two
corrections from David to get right:

- **Clock-driven network** (the normal way to use a frequency-bearing signal):
  the view updates at the clock rate. Measured **exactly 10.0 store updates/s**
  for a 10 Hz network over 4 s. Compute at sensor rate, publish at display rate.
- **Source signal poked with `set()`**: not throttled at all — it fires per
  `set()`, and the engine isolates it in its own island. Measured ~112 updates/s
  when driven at 200 set/s. Declaring a frequency and then calling `set()` on it
  mixes the two models and is not idiomatic.

I originally asserted the first without measuring, then over-corrected to "the
rate does nothing" after testing only the unidiomatic case. Both statements were
wrong in different directions; the table above is what the measurements actually
support.

Method calls are not a third competing abstraction: `ActorInstance::queueCall()`
enqueues a message and returns a Future, so a call *is* an event with a reply
channel. The reply is load-bearing — form submission needs "did that succeed?",
which an event cannot answer. Plain events stay available for scripts that prefer
`when … occurs` and need no result.

The failure mode to avoid is forcing everything through one channel:
clicks-as-signals silently drops occurrences; state-as-events makes the JS side
accumulate state by hand, reimplementing the store badly.

### What M2 settled

- **One host loop, shared.** `VM::setHostEventLoop()` holds exactly one loop, so
  `dom` and `web` each installing their own would silently disable whichever went
  first. Both now install the same singleton (`compiler/web/WebHostLoop.*`), which
  also owns the park/stop lifecycle.
- **Coalescing is real and measured.** Five writes inside one Roxal turn produce
  exactly **one** subscriber notification: dirty roles collect on the VM side and
  flush once per turn, and the JS side batches into a microtask on top. Pinned by
  `wasm/test-web.cjs`.
- **A computed property observes its own backing field only.** A getter derived
  from *other* fields has nothing to watch and cannot auto-fire — `web.notify()`
  is the documented escape hatch. Inherited from the qt design, and worth knowing
  before it looks like a bug.
- **Write against the real grammar.** Roxal uses `this`, not `self`; `event`,
  `type` and `by` are keywords and cannot be parameter names; an untyped parameter
  cannot be indexed, so event handlers need `proc(e :dict):`.

## Lifetime and GC

- Roxal objects exposed to JS need GC roots — a `WebBindHub` singleton holding
  them as traced members, bracketed by module load/shutdown. Straight copy of
  `QtBindHub`.
- JS handles held by Roxal must be released when the `ObjJsValue` is collected.
  Its destructor **queues** a `RELEASE` op — it must not proxy synchronously,
  because it can run on the GC thread.
- Roxal callbacks passed to JS (event listeners) are roots until `UNLISTEN`.

## The 3D seam (design only)

3D bypasses the DOM bridge entirely. The main thread calls
`canvas.transferControlToOffscreen()` **once** and transfers the `OffscreenCanvas`
to the VM worker; from then on the worker holds a real GPU context and drives it
with no proxying and no marshalling.

Emscripten exposes WebGPU as the standard `webgpu.h` C API, so a future `gpu`
module binds a C API rather than JS objects through a handle table. The payoff:
`webgpu.h` has native implementations (Dawn, wgpu-native), so **one Roxal 3D
module gets a browser backend and a native desktop backend from the same
binding** — a much better position than the DOM, which has no native equivalent.
It also gives the Doom port a browser target: FrameView tensor → canvas.

All layer 0 must provide today: an op that can pass transferables to the worker,
and not painting us into a corner on `-sOFFSCREENCANVAS_SUPPORT` /
`specialHTMLTargets`.

## Electron

The browser build runs in Electron unchanged — same page, same wasm; COOP/COEP
are trivial to set on a `file://`-adjacent custom protocol or via
`session.webRequest`. That path costs nothing.

The more interesting long-term option is a **native** Roxal in the Electron main
process driving the same binding surface over IPC, because it buys back fileio,
FFI, DDS and real filesystem access that a browser build structurally cannot
have. Layers 1 and 2 are already written against a transport seam ("execute this
op batch"), so that swap is a transport implementation, not a redesign. Worth
keeping the seam honest even though we are not building it now.

## Build system

Split along the **language** boundary, not the deliverable boundary: the wasm
build is a *configuration* of the same C++ product, but the JS frontend is a
different product with a different toolchain. Those want opposite treatment.

### C++ stays in the root CMakeLists

The wasm build already uses it — `emcmake cmake -B build-wasm-mt` configures the
root project with the emscripten toolchain. Only the final link sat outside, in
`wasm/build.sh`, and that split had a real cost: build.sh scraped ABI defines
back out of the generated `roxal_features.cmake`, because a mismatch there is
silent memory corruption rather than a link error.

That workaround existed only because the link left CMake. So:

```
CMakeLists.txt    option(ROXAL_BUILD_WASM_HOST); if(EMSCRIPTEN) add_subdirectory(wasm)
wasm/CMakeLists.txt   host target: link flags, EXPORTED_FUNCTIONS, --preload-file
```

`wasm/` is a **child** of the root project, which is the direction CMake wants —
locality for the wasm-specific flags, with a single source of truth for the ABI
defines. The host links `roxalcore`, which links `roxal_abi` PUBLIC, so the
defines, include dirs, antlr4 and dataflow all arrive by inheritance. The
scraping is deleted, not reimplemented.

Rejected: an independent CMake project in `wasm/` referencing the main build. It
would have to re-declare sources or defines, which re-creates the ABI-drift
hazard with more ceremony, and reaching *up* from a child means
`ExternalProject`/`FetchContent` on a sibling — slow to iterate on.

Accepted cost: `if(EMSCRIPTEN)` conditionals accumulate in a file already 812
lines long, and CMake gives no help coordinating two build dirs with different
toolchains — each is still configured separately.

### JS gets its own npm workspace

Never driven from CMake. Wrapping npm in `add_custom_command` is a known dead end
— stale dependency tracking, doubled configure time, CI pain that scales with the
JS code. The boundary is an **artifact hand-off**: the JS build consumes
`roxal.js` / `roxal.wasm` as input files.

```
web/
  package.json           @roxal/web — React + Vite workspace
  vite.config.js         COOP/COEP headers (replaces serve.py for app dev)
  scripts/sync-wasm.mjs  copies roxal.js/.wasm/.data from build-wasm-mt/dist
  src/roxal.js           loads the VM, runs the app script
  src/roxal-react.js     useRoxal / useRoxalStore / useRoxalProperty
  src/app.rox.js         the Roxal side of the reference app
  src/App.jsx            the React side
```

Built and browser-verified: a simulated arm with a 20 Hz clock-driven control
loop in Roxal, rendered by React through `useSyncExternalStore`. React → method
call → control loop → signal → store → re-render, all confirmed in Chrome.

Two side benefits: Vite's dev server sets COOP/COEP from config, retiring
`serve.py` and `coi-sw.js` as bespoke tools; and `@roxal/web` as an npm package is
how React developers will expect to consume this.

This is not optional in the way it looks. JSX requires a bundler by definition,
so committing to the React authoring story commits this repo to a real JS
toolchain whether CMake knows about it or not. The choice is only whether to
pretend otherwise.

### Tooling goes in install-deps.sh

`emsdk` and `antlr4-wasm` become first-class targets alongside `eigen`, `antlr4`,
`onnxruntime` — the script already has the target/description/installed-status
structure for exactly this.

## Editor decision

**Monaco**, decided by David. Both Monaco and CodeMirror 6 are MIT and
framework-agnostic (the React wrappers are thin), so this changes no
architecture — only which APIs the IDE work targets:

- syntax highlighting via a **Monarch** tokenizer (there is a live playground at
  microsoft.github.io/monaco-editor/monarch.html for prototyping Roxal's)
- diagnostics via Monaco's **marker** API, fed from the real Roxal compiler
  rather than a reimplemented grammar — the compiler is already in the page

One packaging wrinkle to expect: Monaco ships its language services as separate
web workers, which needs deliberate Vite configuration. Worth resolving early
rather than discovering it late.

## IDE trimmings: what has to come first

Menus and tabs are the visible part, but they are downstream of one thing.

**Tabs are tabs over files, and Roxal has no files in the browser yet.** The wasm
build sets `ROXAL_ENABLE_FILEIO=OFF`, and modules load from a read-only `/stdlib`
preloaded at link time. So a user's second file cannot be `import`ed by their
first: "multiple tabs" today would give you multiple scratchpads that cannot
refer to each other, which is not multi-file development. Menus are mostly file
operations anyway (New / Open / Save / Save As), so they inherit the same
dependency.

The order that follows:

**F1 — a writable filesystem in wasm.** Turn `ROXAL_ENABLE_FILEIO` on and mount
WASMFS + **OPFS** (the Origin Private File System): browser-managed, persistent,
and it offers *synchronous* access handles inside workers, which suits Roxal's
blocking IO model exactly. `wasm/README.md` already notes what this needs —
`AsyncIOManager` running inline rather than on its worker. Measurable win: the
`fileio` tests are currently gated out of the wasm suite, and two of the fifteen
known failures (`import_asset_sibling`, `import_folder_init`) exist purely
because package *directories* cannot be written into the wasm FS.

**F2 — a real local folder.** The **File System Access API**
(`showDirectoryPicker`) is literally "fileio backed by a local folder": the user
picks a directory and the IDE reads and writes it. Chrome and Edge support it;
Firefox and Safari do not, so OPFS stays the working store and the local folder
is an open/save target synced against it. That split also gives a sane answer for
browsers without the API.

**F3 — the chrome.** File tree, tabs, menus. All straightforward once a file is a
real thing, and all guesswork before that.

### Where the REPL fits

After F1, as a panel beside `output` in the right column. It needs a small host
entry point: the VM already has `setupLine()` / `runLine()` for REPL input, but
the wasm host deliberately does not implement it (hence the known `repl_run`
failure). The pairing that makes it worth having is `web.expose` — a REPL that
can inspect and poke the *running* app's exposed state, rather than a detached
scratch buffer.

### Editing the right-hand pane

Not with an HTML editor and a JS editor. The `dom` module already lets a Roxal
script build its own UI, so the right pane should be a container that the script
owns — `dom.document.get_element_by_id("app")` and go. That demonstrates Roxal
rather than working around it, needs no new editors, and matches an eventual IDE
that will not ship an HTML editor either.

## Dataflow introspection

`_df_graphdot()` / `_df_islands()` exist and do dump the live graph, but they are
debug facilities: **the output is not stable and not intended as an API.** Do not
build a UI on them.

**The `inspect` module now exists and is that surface** (see the "Source &
Dataflow Introspection" section of roxal-for-devs.md). For the React Flow
dataflow view: `sig.network()` returns the island as `Network` →
`DataflowNode` → `Port` objects with stable ids (React Flow node keys),
port latency indices, and `src_name`/`src_line`/`src_col` provenance to
correlate graph nodes back to the editor buffer; live values read through
borrowed signal references (`port.sig.value`) that can never tear down
network parts. Writes are AST edits: `inspect.parse` (tolerant mode returns
partial trees for mid-edit buffers, plus positions/deduced types for
intellisense), plain-assignment editing, `inspect.unparse`, and
`inspect.compile`. The remaining work is the JS bridge plumbing and the
React view itself.

## The language service (done)

The editor's squiggles and hover types come from the real compiler front end, not
from a second approximate grammar. There is no new bridge machinery: the service
is an ordinary Roxal object exposed through the state bridge.

```roxal
type Ide object:
  func check(src :string) -> list:            # -> [{line, col, message}]
  func describe(src, line, col) -> dict:      # -> {kind, line, col, type}
web.expose("ide", Ide())
```

`check` is `inspect.parse(src, name, true)` — tolerant mode, so a mid-edit buffer
still yields the error list Monaco's marker API wants. `describe` walks the tree
for the innermost node containing the position and returns its `kind` and, where
the deducer filled it in, `deduced_type`. The editor debounces `check` by 400 ms
(each call is a round trip to the VM thread) and registers a hover provider for
`describe`.

Things that cost time and are easy to hit again:

* **`editor.api.js` registers no contributions.** `registerHoverProvider`
  resolves happily and nothing ever renders. `contrib/hover/browser/hoverContribution.js`
  has to be imported explicitly; `editor.main.js` would work but drags in all
  ~160 contributions and every language.
* **Roxal columns are 0-based, Monaco's are 1-based.**
* **The service arrives long after the editor mounts.** Without a re-check when
  it does, a file that is already broken on load stays unmarked until the user
  types. `rox.roxalStore(name)` also returns a *fresh* handle per call, so the
  prop has to be memoised or the effect re-fires at the network's update rate.
* In tests: `showHover` is a no-op while a hover is visible, so a second lookup
  silently returns the first one's answer unless `hideHover` is triggered first.

**The service is an actor** (`type Ide actor:`), and the bridge knows how to
expose one: `web.expose` accepts an `ActorInstance`, `RoxalStore::invoke`
queues the call onto the actor's thread (`ActorInstance::queueCall`,
`forceCompletionFuture=true`) instead of running it synchronously, and the
host-loop pump polls the pending futures and resolves each JS promise as its
result lands. JS sees no difference — `store.call()` already returned a
Promise. What changes is who waits: measured on the wasm build, a 912-line
parse (269 ms) used to stall the pump for its full duration; as an actor call
the oven store's max update gap stayed at 58 ms while the parse ran. The rules
that come with actor stores: no public properties (the language forbids shared
actor state, so they are methods-only), and no computed accessors (reading one
executes its getter, and the store reads from the VM thread).

**Known limitation, worth fixing before this is a real IDE:** the service lives in
the *user's* script, so a script that fails to compile leaves no store — and the
diagnostics vanish exactly when they matter. The fix is to stop conflating the
two: a small harness script owns the service and runs the user's code through
`inspect.compile`, so the service survives its failure. (Being an actor also
positions it to outlive script re-runs once the harness exists.)

**Open bug:** repeated large parses (~950 lines, 6+ in a row) crash the VM with
`memory access out of bounds` / `unaligned access` — pre-existing, unchanged by
the actor move (same code, different thread), reproducible in
`actor-measure.js`-style node harnesses. Needs an `-sASSERTIONS=2
-sSTACK_OVERFLOW_CHECK=2` build to name the culprit. Until then the editor's
400 ms debounce keeps real-world exposure low. Related: the page has no worker
`onerror` handler, so a VM crash currently looks like a silent freeze — the
crash surfaces only in the browser console.

## The IDE shell (done)

The editor is now a small IDE: a File menu (New/Open/Save/Save As/Delete), a
tab strip, an output pane and a REPL console. Files live under `/data`, which
the wasm host mounts on OPFS, so they survive a reload; the IDE reaches them
through a `Workspace` actor in the web module whose operations are ordinary
`fileio` calls, which means File > Open exercises exactly the API user scripts
get. One Monaco *model* per file (keyed by URI, switched with `setModel`)
gives each tab its own undo history, cursor and markers for free.

Two things learned here are worth carrying forward:

**The IDE must own a parked script.** The host event loop is pumped from the
VM's dispatch loop, so it runs only while a script is running or parked. A
user script that simply ENDS -- a batch script, or one that errors before
`web.serve()` -- leaves nothing pumping, and every store call then hangs
forever with no rejection: dead menus, a dead console, and a Run button that
wedges on its own first call. The fix is an invariant rather than a patch:
`scriptParked()` derives liveness exactly (submitted minus completed) and
`ensureServices()` re-parks the IDE's own bootstrap before Run, before every
File action, after a failed run, and from a watchdog. Store calls also carry
a 20s timeout, because a promise that never settles is indistinguishable from
a slow one. Generalises: any host embedding Roxal behind a UI needs its own
always-parked script, separate from the program being edited.

**A REPL line is its own program, and fatal errors are fatal.** `eval` parses
the line, print-wraps an expression (a compiled program's top level has no
return channel) and runs it via `inspect.compile`. Two consequences follow
from the language, not from this code: a compiled line sees globals and
builtins but NOT the running app's variables or its imports, so there is no
inspecting live state from the console today; and an unresolved name is a
*fatal, non-catchable* runtime error, so a typo like `sfsdfds` would take the
whole app down and no `try/except` in `eval` could stop it. Hence
`sys.defined(name)` and a pre-flight check of the names an expression
mentions -- the console explains instead of crashing. Statements and
declarations still run unchecked, so `var x = typo` remains fatal; the IDE
recovers, but the app restarts.

Making the console able to read live app state, and making unresolved names
catchable, are both language-level questions rather than IDE ones. The first
wants a REPL that compiles into the running program's scope; the second is a
decision about Roxal's error model (most runtime errors are deliberately
fatal). Neither is needed for the demo.

## Repo positioning: keep upstream out of the robotics domain

This is the public upstream repo and it deliberately excludes the robot library,
which lives in roxal-internal (consumed by Future Controller, with a real
hardware controller and a MuJoCo simulator). The WebAssembly work *for robotics*
will live there too.

So examples here should not read as robot-heavy, even though robotics is a main
target of the language. The mechanics we want to show — a setpoint, feedback, a
control law as a signal network, rate-driven publishing — are domain-neutral;
only the naming makes them look robotic. Pick a mechanism outside the robotics
domain and the demo teaches exactly the same things.

The current `Arm` example is fine for now (it is a slider); the naming is the
only robotic part of it, so it is a rename when next touched.

## What is Roxal-on-the-web actually *for*?

Upstream Roxal has no robotics libraries, controller or simulator, so the web
stack needs its own reason to exist. Three candidates, in increasing order of how
much they show off:

**Web apps.** Already possible: `dom` for direct page access, `web` for
state-driven UIs. This is the boring, useful answer.

**2D graphics and games.** A canvas 2D context is reachable through `dom` today
(`canvas.get_context("2d")`), and the bridge's `Bytes` tag was designed so a
tensor can reach `putImageData` without a copy. Phaser and PixiJS are both MIT if
a full engine is wanted, but driving the canvas directly is the more Roxal-native
route.

**3D.** three.js (MIT) through `dom`, or the `gpu` seam already sketched above:
`webgpu.h` on an OffscreenCanvas transferred to the VM worker, which also gets a
native desktop backend from the same binding.

**Not Doom.** It runs at 10–15 FPS native with LTO, and wasm costs another
1.5–2.5×, so it would be 5–8 FPS — a demonstration of the wrong thing.

The rule it illustrates is worth keeping: **do not make Roxal push pixels.** A
software renderer is the worst possible workload for an interpreted VM. Roxal
should own the *logic* — control laws, state machines, dataflow — while the
browser's renderer owns pixels (three.js writing a few dozen transforms per
frame is trivially 60 FPS) and Web Audio owns samples. Same split that makes the
state bridge cheap: Roxal produces control-rate data, the platform does the
per-sample or per-pixel work.

## Milestones

| | | Proves |
|---|---|---|
| ~~M0~~ | **DONE** — `PROXY_TO_PTHREAD` host; VM on a worker, main thread free | The threading model at all |
| M0.5 | **DONE** — inbound script queue: submit from any thread, never blocking | The JS→Roxal direction in its simplest form; what `web.serve()` sits on |
| ~~M1~~ | **DONE** — bridge + `dom`: a Roxal script writes into the page and receives a click | Layer 0 end to end, both directions |
| ~~M2~~ | **DONE** — `web.expose` + `RoxalStore` + React adapter + no-framework demo | The product |
| M3 | Signals → store deltas, coalesced; a live gauge driven by a Roxal signal | Reactivity, the thing that beats writing plain JS |
| ~~M4~~ | **DONE (with M2)** — method calls returning Promises; failures reject | Full round trip |
| ~~M4.5~~ | **DONE** — `web/` Vite+React workspace and a reference app | The adapter proven in a real app, not a file nobody ran |
| M5 | Electron shell running the same bundle | Desktop story |

## Risks and open questions

- ~~**M0 is the real gate.**~~ **Settled.** `PROXY_TO_PTHREAD` works with a VM
  that spawns its own threads during construction, and `VM::markMainThread()`
  latches onto the worker as hoped — the VM's "main thread" means the script
  thread. Measured: the host main thread took 285 timer ticks during the 2.9s a
  script held the VM. `wasm/test-host.cjs` guards it.
- **Sync round-trip latency** is one main-thread task per read. The queued/sync
  split is implemented; layer 2 mostly sidesteps it anyway. Still unmeasured in a
  real browser — the node harness proxies through the same path but with node's
  scheduler, so the number there is not the number that matters.
- **`Atomics.wait` is forbidden on the main thread**, which is precisely why rule 1
  exists. Any design pressure toward a synchronous JS → Roxal call must be
  resisted.
- **Worker cost**: one Web Worker per pthread, megabytes each. An actor-heavy UI
  app may eventually need an M:N scheduler multiplexing Roxal actors onto a small
  pool. Already flagged in `wasm/README.md`; the web work makes it likelier.
- **Snapshot granularity**: whole-object snapshots are simple but re-render more
  than needed for large objects. Per-role subscription is the escape hatch if
  profiling demands it — do not build it up front.
- fileio remains off in the wasm build. If web apps need persistence, that is
  WASMFS + OPFS and a separate piece of work.
