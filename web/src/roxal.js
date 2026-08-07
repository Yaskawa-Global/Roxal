// Load the Roxal VM and run an app script.
//
// roxal.js is an Emscripten MODULARIZE bundle served from public/, not an ES
// module, so it is pulled in with a script tag and hands back a global factory.
// It must NOT go through the bundler: Vite would try to rewrite its Worker
// spawning and its .wasm/.data fetches, both of which Emscripten resolves itself.

let loading = null;

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
export async function startRoxal(source, { expectStore, onOutput } = {}) {
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

        rox.ccall('roxal_submit_source', null, ['string', 'string'], [source, 'app.rox']);

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
export async function runScript(rox, source, { expectStore } = {}) {
    const before = rox.ccall('roxal_completed_count', 'number', [], []);

    rox.ccall('roxal_request_stop', null, [], []);   // harmless if nothing is parked

    const stopBy = Date.now() + 10000;
    while (rox.ccall('roxal_completed_count', 'number', [], []) === before) {
        if (Date.now() > stopBy) throw new Error('the running script did not stop');
        await new Promise(r => setTimeout(r, 20));
    }

    const ranBefore = rox.ccall('roxal_completed_count', 'number', [], []);
    rox.ccall('roxal_submit_source', null, ['string', 'string'], [source, 'app.rox']);

    // The new script parks in web.serve() rather than completing, so wait for the
    // store -- and treat completion as failure, since a script that COMPLETED
    // never reached serve(): it had a compile or runtime error.
    const deadline = Date.now() + 20000;
    for (;;) {
        if (expectStore && rox.roxalStoreNames().includes(expectStore)) return;
        if (rox.ccall('roxal_completed_count', 'number', [], []) > ranBefore)
            throw new Error('script ended without exposing "' + expectStore + '" — see stdout');
        if (Date.now() > deadline) throw new Error('timed out starting the script');
        await new Promise(r => setTimeout(r, 20));
    }
}
