// Load a Roxal VM and run an app script.
//
// The VM is reached through a host (web/src/lib/host.js): the wasm build in
// this tab, or a native `roxal --web-host` process over a WebSocket when the
// page is opened with `?host=ws://127.0.0.1:8765`. Everything here is written
// against the host interface; nothing below knows which one it got.
//
// roxal.js (the Emscripten MODULARIZE bundle) is served from public/, not an ES
// module, so it is pulled in with a script tag and hands back a global factory.
// It must NOT go through the bundler: Vite would try to rewrite its Worker
// spawning and its .wasm/.data fetches, both of which Emscripten resolves itself.

import { installNnProvider } from './nn-provider.js';
import { WasmHost, SocketHost } from './lib/host.js';

let loading = null;

// Every submitted script either PARKS (web.serve/dom.run) or completes, so
// submitted-minus-completed is exactly "is something alive to pump the host
// loop". Store calls are only serviced while that holds -- see scriptParked().
let submitted = 0;

function loadFactory() {
    if (window.createRoxal) return Promise.resolve(window.createRoxal);
    return new Promise((resolve, reject) => {
        const tag = document.createElement('script');
        tag.src = '/roxal.js';
        tag.onload = () => resolve(window.createRoxal);
        tag.onerror = () => reject(new Error('failed to load /roxal.js — run `npm run sync-wasm`'));
        document.head.appendChild(tag);
    });
}

// Diagnostic switches carried in the URL, applied to whichever host we got:
// ?nogc=1 disables the collector for crash triage (mirrors the native --nogc
// flag; memory then only grows), the rest are GC and forensic knobs.
function applyUrlConfig(host) {
    try {
        const qs = new URLSearchParams(location.search);
        if (qs.has('nogc')) host.config('gc.disabled', 'true');
        if (qs.has('gcthreshold')) host.config('gc.threshold', qs.get('gcthreshold'));
        if (qs.has('gcshadow')) host.config('env.ROXAL_GC_SHADOW_SCAN', qs.get('gcshadow'));
        if (qs.has('fcflags')) host.config('forensic.flags', qs.get('fcflags'));
        if (qs.has('gcprecise')) host.config('env.ROXAL_GC_CONSERVATIVE', '0');
    } catch (e) {
        // A missing export here means the treatment you asked for in the
        // URL did NOT apply -- an entire validation series was once
        // invalidated by exactly this being swallowed. Shout.
        console.error('runtime config unavailable — URL GC switches ignored:', e);
    }
}

/**
 * Boot the VM and run `source` as the app script.
 *
 * Resolves once the script has published `expectStore` via web.expose(), because
 * a UI has nothing to render before that. The script itself keeps running — it
 * ends in web.serve(), which parks the VM waiting for UI events.
 *
 * `hostUrl` selects a native host (`ws://...`); without it the wasm VM boots
 * in this tab.
 *
 * @returns {Promise<{rox: object, output: () => string}>}
 */
export async function startRoxal(source, { expectStore, onOutput, name = '<script>', hostUrl = null } = {}) {
    if (loading) return loading;

    loading = (async () => {
        let buffer = '';
        // `text` is raw output, newlines included, exactly as the VM wrote it.
        const append = (text, isErr) => {
            buffer += (isErr ? '[stderr] ' : '') + text;
            // Mirror VM stderr to the devtools console: aborts (stack-overflow
            // canaries, assertions) are written there by the runtime, and the
            // output pane can be gone or truncated by the time they matter.
            if (isErr) console.error('[VM stderr]', text.replace(/\n$/, ''));
            onOutput?.(buffer);
        };

        let rox;
        if (hostUrl) {
            const host = new SocketHost(hostUrl);
            host.onOutput(append);
            const hello = await host.connect();
            // The host may already be inside a script from a previous page
            // (a parked app survives a reload); count it so the liveness
            // logic knows something is alive to pump.
            submitted = hello.completed + (hello.running ? 1 : 0);
            rox = host;
        } else {
            if (!self.crossOriginIsolated)
                throw new Error(
                    'not cross-origin isolated — COOP/COEP headers are missing, so ' +
                    'SharedArrayBuffer is unavailable and the VM cannot spawn its threads');

            const createRoxal = await loadFactory();
            const module = await createRoxal({
                print: t => append(t + '\n', false),
                printErr: t => append(t + '\n', true),
            });
            // ai.nn's backend: scripts that import it get onnxruntime-web
            // (WebGPU when available). Registration is cheap; loading is lazy.
            installNnProvider(module);
            rox = new WasmHost(module);
        }
        applyUrlConfig(rox);

        submitted++;
        rox.submit(source, name);

        if (expectStore) {
            // Poll rather than await: submitting is deliberately fire-and-forget,
            // because the browser main thread must never block on the VM.
            const deadline = Date.now() + 30000;
            while (!rox.roxalStoreNames().includes(expectStore)) {
                if (Date.now() > deadline)
                    throw new Error(`the script never exposed a store named "${expectStore}"`
                                  + (buffer ? `\n\n${buffer}` : ''));
                await new Promise(r => setTimeout(r, 20));
            }
        }

        return { rox, output: () => buffer };
    })();

    return loading;
}

/**
 * Displace whatever script currently owns the VM: ask a web.serve() park to
 * return (graceful), and if the script is still alive after `graceMs` --
 * a batch loop never parks in serve -- interrupt it (a clean VM exit of
 * that run; the host resets state before the next).
 */
export async function stopCurrent(rox, { graceMs = 1500, deadlineMs = 12000 } = {}) {
    const before = rox.completedCount();
    if (!scriptParked(rox)) return;                     // nothing alive
    rox.requestStop();
    const start = Date.now();
    let escalated = false;
    while (rox.completedCount() === before) {
        if (!escalated && Date.now() - start > graceMs) {
            rox.interrupt();
            escalated = true;
        }
        if (Date.now() - start > deadlineMs)
            throw new Error('the running script did not stop');
        await new Promise(r => setTimeout(r, 20));
    }
}

/**
 * Re-run an edited script against the already-running VM.
 *
 * A parked app (web.serve) owns the VM thread, so a newly submitted script would
 * queue behind it forever. Ask the parked script to return first, wait for it to
 * finish, then submit. Re-exposing the same store name replaces it, so the edited
 * object takes effect rather than the old one being silently reused.
 */
export async function runScript(rox, source,
                                { expectStore, assumeStopped, name = '<script>',
                                  superseded } = {}) {
    // `superseded` lets the caller displace a run that is still waiting
    // to start.  A batch script never exposes a store, so this wait runs
    // the full deadline; without a way out, anything the user does in
    // those 20s (press Run again, switch on debugging) is swallowed.
    const givenUp = () => Boolean(superseded && superseded());
    const supersededError = () =>
        Object.assign(new Error('superseded by a newer run'), { superseded: true });
    if (givenUp()) throw supersededError();
    if (!assumeStopped) {
        await stopCurrent(rox);
    }

    const ranBefore = rox.completedCount();
    // Generation, not presence: the JS registry keeps a stale record for the
    // store across runs, so .includes(name) is true the moment the OLD run has
    // ever exposed it. Only a fresh DEFINE proves the new script reached
    // serve().
    const genBefore = rox.roxalStoreGeneration(expectStore);
    submitted++;
    rox.submit(source, name);

    // The new script parks in web.serve() rather than completing, so wait for the
    // store -- and treat completion as failure, since a script that COMPLETED
    // never reached serve(): it had a compile or runtime error.
    const deadline = Date.now() + 20000;
    for (;;) {
        if (expectStore && rox.roxalStoreGeneration(expectStore) > genBefore) return;
        if (rox.completedCount() > ranBefore) {
            // Completing is not automatically a failure: a script with no
            // web.serve() is an ordinary batch script that ran and finished.
            // Only the exit code distinguishes that from a script that died,
            // so say which -- "ended without exposing a store" reads as a
            // defect either way, and for a print-and-exit script it is not one.
            const rc = rox.lastResult();
            throw Object.assign(new Error(rc === 0
                ? 'the script ran to completion — it never called web.serve(), '
                  + 'so there is no live app to interact with'
                : 'the script stopped with an error — see the output pane'),
                { scriptEnded: true, rc });
        }
        if (givenUp()) throw supersededError();
        if (Date.now() > deadline) throw new Error('timed out starting the script');
        await new Promise(r => setTimeout(r, 20));
    }
}


/**
 * Is a script currently running or parked?
 *
 * This is the IDE's liveness invariant, not a curiosity: the host event loop is
 * pumped from the VM's dispatch loop, so with no script alive NOTHING services
 * store calls, DOM events or store patches. A user script that simply ends --
 * a batch script, or one that never reaches web.serve() -- leaves the IDE with
 * dead menus, a dead console and a Run button that hangs on its first call.
 */
export function scriptParked(rox) {
    return submitted > rox.completedCount();
}

/**
 * Guarantee that something is parked, re-parking `bootstrap` if not.
 *
 * The services an IDE needs (its language service, its file access) belong to
 * the IDE, not to the program being edited -- so when the edited program ends,
 * the IDE puts its own script back. Returns true if it had to intervene.
 */
export async function ensureServices(rox, bootstrap, { expectStore = 'workspace' } = {}) {
    if (scriptParked(rox)) return false;
    await runScript(rox, bootstrap, { expectStore, assumeStopped: true });
    return true;
}
