// Browser NN provider for Roxal's ai.nn module.
//
// Wraps the shared provider factory (wasm/roxal-nn-provider.mjs) around
// onnxruntime-web: the WebGPU EP when the machine has an adapter, the wasm
// EP (CPU) otherwise. Everything loads lazily -- ort's JS lands in its own
// chunk on first session create, and its ~26MB runtime .wasm is only fetched
// then too -- so pages that never touch ai.nn pay nothing.
//
// The runtime .wasm files are served same-origin from /ort/ (copied out of
// node_modules by scripts/sync-wasm.mjs) because the page is cross-origin
// isolated: a CDN copy without CORP headers would be blocked by COEP.

import makeRoxalNN from '../../wasm/roxal-nn-provider.mjs';

let providerPromise = null;

// Why the page is (or is not) using the GPU. Falling back silently is the
// worst outcome: inference still works, just slower, and nothing says so --
// which is exactly how "it says cpu on my machine" becomes a mystery. The
// panel reads this; the console gets it too.
const status = { device: null, webgpu: false, reason: 'not started' };

export function nnStatus() { return { ...status }; }

async function probeWebGpu() {
    if (!navigator.gpu) {
        // Chrome exposes navigator.gpu only in a secure context, and disables
        // it entirely when GPU acceleration is off.
        status.reason = window.isSecureContext
            ? 'this browser has no WebGPU (navigator.gpu is undefined)'
            : 'WebGPU needs a secure context (https or localhost)';
        return false;
    }
    let adapter = null;
    try {
        adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    } catch (e) {
        status.reason = 'requestAdapter failed: ' + (e?.message || e);
        return false;
    }
    if (!adapter) {
        // The usual cause on Linux: the GPU is on Chrome's blocklist, so
        // WebGPU exists but yields no adapter. chrome://gpu says which.
        status.reason = 'no WebGPU adapter — see chrome://gpu (on Linux, try '
                      + '--enable-features=Vulkan --use-vulkan=native --ignore-gpu-blocklist)';
        return false;
    }
    const info = adapter.info || {};
    status.reason = 'adapter: ' + [info.vendor, info.architecture].filter(Boolean).join(' ');
    return true;
}

async function load() {
    const ort = await import('onnxruntime-web');
    ort.env.wasm.wasmPaths = '/ort/';
    status.webgpu = await probeWebGpu();
    const inner = makeRoxalNN(ort, { webgpu: status.webgpu });
    return {
        ...inner,
        // Record where the session actually landed. 'auto' can still fall back
        // to the CPU after a successful adapter probe (an EP that fails to
        // build), and that is worth reporting too.
        async create(bytes, device) {
            const meta = await inner.create(bytes, device);
            status.device = meta.device;
            if (status.webgpu && meta.device !== 'webgpu')
                status.reason = 'the WebGPU execution provider failed to build; using the CPU';
            console.info('[roxal ai.nn] running on ' + meta.device + ' — ' + status.reason);
            return meta;
        },
    };
}

/**
 * Start loading the runtime BEFORE a script needs it.
 *
 * `Model()` blocks the VM thread until the provider answers -- it has to, the
 * script continues with the session -- and while it is blocked nothing pumps
 * store calls, so a first-visit download of ORT's ~26MB runtime freezes the
 * IDE's menus and Run button for as long as the network takes. Fetching the
 * binary here puts it in the HTTP cache first, so ORT's own fetch inside that
 * blocking window is a cache hit.
 *
 * Fire-and-forget: every failure here is recoverable (the real load will
 * simply fetch it again), so nothing is awaited and nothing is reported.
 */
export function warmNnProvider() {
    const get = () => providerPromise ??= load();
    get().then(() => {
        // jsep is the WebGPU-capable build; the plain one backs the CPU EP.
        const file = status.webgpu ? 'ort-wasm-simd-threaded.jsep.wasm'
                                   : 'ort-wasm-simd-threaded.wasm';
        return fetch('/ort/' + file, { cache: 'force-cache' });
    }).catch(() => {});
}

/**
 * Register the NN provider on an Emscripten module instance. The wasm-side
 * bridge dispatches ai.nn's create/run/close requests to `rox.roxalNN`.
 */
export function installNnProvider(rox) {
    const get = () => providerPromise ??= load();
    rox.roxalNN = {
        create: async (bytes, device) => (await get()).create(bytes, device),
        run: async (id, feeds) => (await get()).run(id, feeds),
        close: async (id) => { if (providerPromise) (await providerPromise).close(id); },
    };
}
