// Load the Roxal VM and run an app script.
//
// roxal.js is an Emscripten MODULARIZE bundle served from public/, not an ES
// module, so it is pulled in with a script tag and hands back a global factory.
// It must NOT go through the bundler: Vite would try to rewrite its Worker
// spawning and its .wasm/.data fetches, both of which Emscripten resolves itself.

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

/**
 * Boot the VM and run `source` as the app script.
 *
 * Resolves once the script has published `expectStore` via web.expose(), because
 * a UI has nothing to render before that. The script itself keeps running — it
 * ends in web.serve(), which parks the VM waiting for UI events.
 *
 * @returns {Promise<{rox: object, output: () => string}>}
 */
export async function startRoxal(source, { expectStore, onOutput, name = '<script>' } = {}) {
    if (loading) return loading;

    loading = (async () => {
        if (!self.crossOriginIsolated)
            throw new Error(
                'not cross-origin isolated — COOP/COEP headers are missing, so ' +
                'SharedArrayBuffer is unavailable and the VM cannot spawn its threads');

        const createRoxal = await loadFactory();
        let buffer = '';
        const append = (text, isErr) => {
            buffer += (isErr ? '[stderr] ' : '') + text + '\n';
            onOutput?.(buffer);
        };

        const rox = await createRoxal({
            print: t => append(t, false),
            printErr: t => append(t, true),
        });

        submitted++;
        rox.ccall('roxal_submit_source', null, ['string', 'string'], [source, name]);

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
 * Re-run an edited script against the already-running VM.
 *
 * A parked app (web.serve) owns the VM thread, so a newly submitted script would
 * queue behind it forever. Ask the parked script to return first, wait for it to
 * finish, then submit. Re-exposing the same store name replaces it, so the edited
 * object takes effect rather than the old one being silently reused.
 */
export async function runScript(rox, source, { expectStore, assumeStopped, name = '<script>' } = {}) {
    if (!assumeStopped) {
        const before = rox.ccall('roxal_completed_count', 'number', [], []);

        rox.ccall('roxal_request_stop', null, [], []);   // harmless if nothing is parked

        const stopBy = Date.now() + 10000;
        while (rox.ccall('roxal_completed_count', 'number', [], []) === before) {
            if (Date.now() > stopBy) throw new Error('the running script did not stop');
            await new Promise(r => setTimeout(r, 20));
        }
    }

    const ranBefore = rox.ccall('roxal_completed_count', 'number', [], []);
    // Generation, not presence: the JS registry keeps a stale record for the
    // store across runs, so .includes(name) is true the moment the OLD run has
    // ever exposed it. Only a fresh DEFINE proves the new script reached
    // serve().
    const genBefore = rox.roxalStoreGeneration?.(expectStore) ?? 0;
    submitted++;
    rox.ccall('roxal_submit_source', null, ['string', 'string'], [source, name]);

    // The new script parks in web.serve() rather than completing, so wait for the
    // store -- and treat completion as failure, since a script that COMPLETED
    // never reached serve(): it had a compile or runtime error.
    const deadline = Date.now() + 20000;
    for (;;) {
        if (expectStore && (rox.roxalStoreGeneration?.(expectStore) ?? 0) > genBefore) return;
        if (rox.ccall('roxal_completed_count', 'number', [], []) > ranBefore) {
            // Completing is not automatically a failure: a script with no
            // web.serve() is an ordinary batch script that ran and finished.
            // Only the exit code distinguishes that from a script that died,
            // so say which -- "ended without exposing a store" reads as a
            // defect either way, and for a print-and-exit script it is not one.
            const rc = rox.ccall('roxal_last_result', 'number', [], []);
            throw Object.assign(new Error(rc === 0
                ? 'the script ran to completion — it never called web.serve(), '
                  + 'so there is no live app to interact with'
                : 'the script stopped with an error — see the output pane'),
                { scriptEnded: true, rc });
        }
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
    return submitted > rox.ccall('roxal_completed_count', 'number', [], []);
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
