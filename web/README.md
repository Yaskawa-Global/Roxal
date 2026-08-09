# Roxal on the web — the JS side

Roxal owns state and logic; your view layer subscribes to it. This directory is
the JavaScript half: the store contract, the React adapter, and a reference app.

See `../web-integration-plan.md` for the architecture and `../wasm/README.md` for
the wasm build.

## Running the reference app

```bash
./wasm/build.sh          # from the repo root: build the VM (CMake owns the C++)
cd web
npm install
npm run dev              # http://localhost:5173
```

`npm run build` produces a static bundle in `dist/`; `npm run preview` serves it.
Both dev and preview send the COOP/COEP headers, so `serve.py` is not needed here.

The app drives a simulated robot arm: Roxal owns the state, a 20 Hz clock-driven
control loop, and every action; React renders it and knows nothing else. Press
"go to 75" and the position converges under Roxal's control while React re-renders
from the store.

### How the two build systems meet

`npm run sync-wasm` copies `roxal.js` / `roxal.wasm` / `roxal.data` out of
`build-wasm-mt/dist/` into `public/`. That copy **is** the boundary: CMake owns
the C++ and produces artifacts, the JS toolchain owns the app and consumes them.
Neither build drives the other — wrapping one in the other is how you get stale
dependencies that neither tool can see. It runs automatically before `dev` and
`build`, and re-copies whenever the C++ output is newer.

Point `ROXAL_WASM_DIST` at another directory to use a different build.

`roxal.js` is loaded with a script tag from `public/`, deliberately outside the
bundler: it is an Emscripten MODULARIZE bundle that spawns Workers and fetches
its own `.wasm`/`.data`, and Vite must not rewrite any of that.

### If `npm run dev` fails with ENOSPC

That is the Linux inotify watch limit, not the app:

```bash
sudo sysctl fs.inotify.max_user_instances=512   # or max_user_watches
```

`npm run preview` needs no watcher and works regardless.

## The store contract

`web.expose("app", obj)` in Roxal publishes an object. On the JS side it becomes:

```js
const store = rox.roxalStore('app');

store.subscribe(fn)        // -> unsubscribe; fn receives the new snapshot
store.getSnapshot()        // -> frozen object; STABLE identity until data changes
store.call(method, ...args)// -> Promise, settled by the VM
store.set(prop, value)     // write back into Roxal
store.methods              // names of the callable methods
```

Four functions. That is the whole contract, and it is deliberately not shaped for
any one framework.

**Snapshot identity is the load-bearing detail.** `getSnapshot()` returns the same
object until something actually changes, and a new frozen object when it does.
React's `useSyncExternalStore` re-renders forever without that guarantee; Svelte
and Vue do not care, because they consume the pushed value. Satisfying the
strictest consumer satisfies the rest.

**Updates are coalesced.** Several Roxal writes in one turn produce one
notification, and the JS side batches into a microtask on top of that. A burst of
state changes costs one render, not one per assignment.

## Adapters

| Framework | Adapter |
|---|---|
| React 18+ | `roxal-react.js` here — `useSyncExternalStore` |
| Svelte | none needed: its store contract *is* `subscribe` |
| Vue 3 | a `shallowRef` swapped in `subscribe` |
| Solid / Preact signals | an external signal set from `subscribe` |
| Lit / web components | `subscribe` → `requestUpdate()` |
| No framework | `subscribe` → your own render function |

Only the React one ships, because it is the authoring story we chose. The others
are each a handful of lines; write one where you need it rather than waiting for
this repo to bless it.

```js
// Svelte, in full:
export const app = { subscribe: fn => {
    fn(rox.roxalStore('app').getSnapshot());
    return rox.roxalStore('app').subscribe(fn);
} };
```

```js
// No framework at all — see ../wasm/store-demo.html for a working page:
const store = rox.roxalStore('app');
store.subscribe(s => { document.getElementById('count').textContent = s.count; });
document.getElementById('inc').onclick = () => store.call('bump', 1);
```

## Signals

A `signal` property crosses as its current value, and the store observes the
signal itself — necessary, because the property slot holds the same signal object
forever while the value changes inside it.

**A clock-driven signal updates the view at its clock rate.** This is the
idiomatic form: the signal is computed by the dataflow engine in a network driven
by `clock(freq=N)`, not poked with `set()`.

```roxal
var display = clock(freq=10)

type App object:
  var temp :signal = signal(10, 0)

app.temp <- display * 2        // engine computes on 10 Hz boundaries
```

Measured: **exactly 10.0 store updates/s** over a 4 s window. Compute at sensor
rate, publish at display rate, and the view knows nothing about either.

**A source signal poked with `set()` is not throttled.** Declaring a frequency
does not rate-limit it — a non-periodic signal fires whenever `set()` is called,
and the engine places it in an isolated island of its own. Measured: a
`signal(10, 0)` driven at 200 `set()`/s produced ~112 store updates/s. Mixing the
two models (a frequency-bearing signal you also `set()`) is not idiomatic; use a
frequency-less signal for event-style sources.

Two ceilings apply on top of either model: the store coalesces changes within a
pump interval into one update, and the host pump runs at ~125/s, so that is the
practical maximum notification rate regardless of what you publish.

## Neural networks (ai.nn)

The wasm build has no ONNX Runtime linked in. `ai.nn` instead calls out to a
**host NN provider** — `Module.roxalNN` — which the page registers at boot
(`src/nn-provider.js` wraps the shared factory in `wasm/roxal-nn-provider.mjs`
around onnxruntime-web). The same factory backs the node test runner, so the
`nn_*` suite runs in wasm against the **native** expected output: numerical
parity is a test result, not a hope.

Device strings map to execution providers: `'auto'` takes WebGPU when the
browser offers an adapter and falls back to the wasm EP, `'webgpu'` demands
it, `'cpu'` forces the wasm EP. `model.device()` reports where the session
actually landed, `'webgpu'` or `'cpu'`.

**When it says `cpu` on a machine with a GPU**, the provider records why —
check the console line (`[roxal ai.nn] running on …`) or hover the panel's
device chip. On Linux the usual cause is Chrome's GPU blocklist: `chrome://gpu`
will say so, and launching with `--enable-features=Vulkan --use-vulkan=native
--ignore-gpu-blocklist` gets an adapter. WebGPU also requires a **secure
context**, so `navigator.gpu` is absent on a plain-http origin.

ORT's runtime binaries are served same-origin from `/ort/` (staged by
`scripts/sync-wasm.mjs`, uploaded by `deploy.sh`): the page is cross-origin
isolated, so the CDN copies ORT would otherwise fetch are blocked by COEP.
Nothing loads until a script actually creates a model.

## The editor

A plain Monaco editor over the Roxal source, with a Run button that re-runs the
edited script against the live VM.

Deliberately language-less for this first pass: no Monarch tokenizer, no
IntelliSense, no navigation. You still get multi-cursor, find/replace, undo,
bracket matching and column select, which is most of why one embeds a real editor
rather than a `<textarea>`. Highlighting comes next; navigation and completion
need the AST surfaced to JS, which is separate work.

Three Vite details, each of which cost a build failure:

- Import from `monaco-editor/esm/vs/editor/editor.api.js`, not `monaco-editor`.
  The package entry pulls in every language contribution and the LSP feature
  layer — ~4MB versus ~2.8MB for the core editor we actually use.
- Both that and the worker are imported by **relative path** into
  `node_modules/`. The bare specifier fails: `?worker` is carried into
  monaco-editor's package `exports` resolution, which knows nothing about query
  strings; `new URL(bare, import.meta.url)` does not resolve bare specifiers; and
  a Vite alias never matches because aliasing happens before the query is
  stripped.
- The worker must be **same-origin**. The page is cross-origin isolated, so
  Monaco's default of fetching workers from a CDN is blocked outright.

### Re-running an edited script

`runScript()` asks the parked app to return (`roxal_request_stop`), waits for it
to finish, then submits the new source. Re-exposing an existing store name
**replaces** it, so an edited object takes effect — reusing the old one would
silently ignore the user's edit, the worst failure mode a live editor can have.

## Packaging

Not an npm package yet. When there is a real app to build, this becomes a
workspace (`@roxal/web`) consuming `roxal.js` / `roxal.wasm` as prebuilt inputs —
the JS toolchain owns its own build, and the boundary with CMake stays an artifact
hand-off. See the build-system section of the plan.
