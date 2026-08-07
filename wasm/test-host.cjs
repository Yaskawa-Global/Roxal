// Host-level tests for the wasm build: the threading model and the inbound
// script queue. These are about the HOST, not the language -- run-tests.cjs
// covers the language by running Roxal's own tests/*.rox suite.
//
//   cd build-wasm-mt/dist && node ../../wasm/test-host.cjs
//
// What is being protected here:
//   * main() -- and therefore the VM -- runs on a worker, not the host main
//     thread. Roxal scripts block by nature, so running the VM on the browser
//     main thread would freeze the page.
//   * VM::markMainThread() latches onto that worker. The VM's "main thread"
//     means the script thread, which everything downstream assumes.
//   * The host main thread keeps servicing its event loop while a script runs.
//   * Scripts can be submitted from another thread without blocking it, and
//     complete in order.

const path = require('path');
const createRoxal = require(path.resolve(process.cwd(), 'roxal.js'));

const INFO_IS_BROWSER_MAIN = 1 << 0;
const INFO_VM_MAIN_THREAD  = 1 << 1;
const INFO_LATCHED         = 1 << 2;

// Long enough to observe whether the host main thread is stuck behind the VM.
const BUSY = `
var x = 0
for i in range(..<2000000):
  x = x + i
print("busy done")
`;

const results = [];
const check = (name, ok, detail) => results.push([name, !!ok, detail || '']);
const sleep = ms => new Promise(r => setTimeout(r, ms));

let out = '', err = '';
const Module = {
    // No `arguments`: main() then serves the inbound queue instead of running one
    // script and exiting.
    print:    t => { out += t + '\n'; },
    printErr: t => { err += t + '\n'; },
    onAbort:  w => { err += '[abort] ' + w + '\n'; },
};

async function waitFor(pred, timeoutMs, what) {
    const deadline = Date.now() + timeoutMs;
    while (!pred()) {
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(10);
    }
}

(async () => {
    const m = await createRoxal(Module);
    const submit = (src, name) =>
        m.ccall('roxal_submit_source', null, ['string', 'string'], [src, name]);
    const completed = () => m.ccall('roxal_completed_count', 'number', [], []);

    // --- inbound queue, and main-thread responsiveness while it runs ---------
    let ticks = 0;
    const timer = setInterval(() => { ticks++; }, 10);
    const t0 = Date.now();
    submit(BUSY, 'busy.rox');                 // must not block this thread
    const submitCost = Date.now() - t0;
    await waitFor(() => completed() >= 1, 60000, 'the busy script');
    const elapsed = Date.now() - t0;
    clearInterval(timer);

    check('submit does not block the caller', submitCost < 100, `${submitCost}ms`);
    check('script ran to completion', out.includes('busy done'));
    check('host main thread stayed responsive', ticks > 0, `${ticks} ticks in ${elapsed}ms`);

    // --- thread identity ----------------------------------------------------
    const info = m.ccall('roxal_thread_info', 'number', [], []);
    check('main() latched thread info', info & INFO_LATCHED, `info=0b${info.toString(2)}`);
    check('VM thread is NOT the host main thread', !(info & INFO_IS_BROWSER_MAIN));
    check('VM::onMainThread() true on the VM thread', info & INFO_VM_MAIN_THREAD);

    // --- a second script, same VM, and ordering -----------------------------
    submit('print("second")\n', 'second.rox');
    submit('print("third")\n', 'third.rox');
    await waitFor(() => completed() >= 3, 30000, 'the queued scripts');
    check('VM outlives one script', out.includes('second') && out.includes('third'));
    check('queued scripts run in order',
          out.indexOf('second') < out.indexOf('third'));
    check('last result reported', m.ccall('roxal_last_result', 'number', [], []) === 0);

    m.ccall('roxal_quit', null, [], []);

    if (err.trim()) console.log('--- stderr ---\n' + err.trim());
    let failed = 0;
    for (const [name, ok, detail] of results) {
        if (!ok) failed++;
        console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   (' + detail + ')' : ''}`);
    }
    console.log(failed ? `\n${failed}/${results.length} FAILED` : `\nall ${results.length} host checks passed`);
    process.exit(failed ? 1 : 0);
})().catch(e => {
    console.error('FAILED: ' + (e && e.stack || e));
    if (err.trim()) console.error('--- stderr ---\n' + err.trim());
    process.exit(1);
});
