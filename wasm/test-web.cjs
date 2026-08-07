// Tests for the `web` module: the state bridge Roxal exposes to a UI.
//
//   cd build-wasm-mt/dist && node ../../wasm/test-web.cjs
//
// This is the layer-2 contract, so the tests are about the contract rather than
// about any framework:
//
//   subscribe(fn) -> unsubscribe        fn receives the new snapshot
//   getSnapshot() -> frozen object      STABLE identity until something changes
//   call(m, ...a) -> Promise            settled by the VM
//   set(prop, v)                        write back into Roxal
//
// Snapshot stability is the strictest requirement (React's useSyncExternalStore
// re-renders forever without it), so it is tested explicitly rather than assumed.

const path = require('path');

const results = [];
const check = (name, ok, detail) => results.push([name, !!ok, detail || '']);
const sleep = ms => new Promise(r => setTimeout(r, ms));

let out = '', err = '';
const Module = {
    arguments: [],
    print:    t => { out += t + '\n'; },
    printErr: t => { err += t + '\n'; },
    onAbort:  w => { err += '[abort] ' + w + '\n'; },
};

function submit(m, src, name) {
    const before = m.ccall('roxal_completed_count', 'number', [], []);
    m.ccall('roxal_submit_source', null, ['string', 'string'], [src, name]);
    return async () => {
        const deadline = Date.now() + 60000;
        while (m.ccall('roxal_completed_count', 'number', [], []) <= before) {
            if (Date.now() > deadline) throw new Error('timed out running ' + name);
            await sleep(5);
        }
        return m.ccall('roxal_last_result', 'number', [], []);
    };
}

async function waitFor(pred, ms, what) {
    const deadline = Date.now() + ms;
    while (!pred()) {
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(10);
    }
}

const APP = `
import web

type App object:
  var count :int = 0
  var label :string = "start"
  var ratio :real = 0.5
  var readonly_note :string = "fixed"

  proc bump(amount :int):
    this.count = this.count + amount
    // doubled is computed from count, not from its own backing field, so it has
    // nothing to auto-observe -- notify() is the documented way to say so.
    web.notify("app", "doubled")

  // A computed (accessor) property, not a method: read through its getter.
  var doubled :int = 0:
    get:
      return this.count * 2

  proc set_label(s :string):
    this.label = s

  // Several writes in ONE turn -- the store must coalesce them into one
  // notification, not one per assignment.
  func describe() -> string:
    return "count=" + this.count

  proc burst():
    this.count = this.count + 1
    this.label = "a"
    this.ratio = 0.1
    this.label = "b"
    this.ratio = 0.2

var app = App()
web.expose("app", app)
print("exposed")
web.serve(6)
print("served")
`;

(async () => {
    const createRoxal = require(path.resolve(process.cwd(), 'roxal.js'));
    const m = await createRoxal(Module);

    const done = submit(m, APP, 'app.rox');

    // --- store appears with its initial snapshot ---------------------------
    await waitFor(() => m.roxalStoreNames().includes('app'), 30000, 'the store to be defined');
    const store = m.roxalStore('app');
    let snap = store.getSnapshot();

    check('store publishes its properties', snap.count === 0 && snap.label === 'start' && snap.ratio === 0.5,
          JSON.stringify(snap));
    check('store advertises its methods',
          store.methods.includes('bump') && store.methods.includes('set_label'),
          JSON.stringify(store.methods));
    check('computed property is published', 'doubled' in snap,
          Object.keys(snap).join(','));

    // --- snapshot identity is stable ---------------------------------------
    // The React requirement: repeated reads with nothing changed must return the
    // SAME object, or useSyncExternalStore loops forever.
    check('getSnapshot is referentially stable', store.getSnapshot() === snap);
    check('snapshot is frozen', Object.isFrozen(snap));

    // --- method call: JS -> Roxal, with a result ---------------------------
    let notifications = 0;
    let lastSeen = null;
    const unsub = store.subscribe(s => { notifications++; lastSeen = s; });

    const bumped = await store.call('bump', 5);
    check('method call resolves', bumped === null || bumped === undefined, JSON.stringify(bumped));

    await waitFor(() => store.getSnapshot().count === 5, 15000, 'count to reach 5');
    snap = store.getSnapshot();
    check('Roxal state change reaches the store', snap.count === 5, JSON.stringify(snap.count));
    check('snapshot identity CHANGES when data changes', snap !== lastSeen || notifications > 0,
          'notifications=' + notifications);
    check('computed property tracks its backing field', snap.doubled === 10,
          JSON.stringify(snap.doubled));

    // --- a method returning a value ----------------------------------------
    const dbl = await store.call('describe');
    check('method return value crosses back', dbl === 'count=5', JSON.stringify(dbl));

    // --- writes: JS -> Roxal ------------------------------------------------
    store.set('label', 'from JS');
    await waitFor(() => store.getSnapshot().label === 'from JS', 15000, 'the write to land');
    check('JS write reaches Roxal and echoes back', store.getSnapshot().label === 'from JS');

    // --- coalescing ---------------------------------------------------------
    // Five writes inside ONE Roxal turn must produce ONE notification, not five.
    // This is what keeps a fast-changing value cheap to display.
    const before = notifications;
    await store.call('burst');
    await waitFor(() => store.getSnapshot().ratio === 0.2, 15000, 'the burst to land');
    await sleep(150);
    const fired = notifications - before;
    check('5 writes in one turn coalesce to 1 notification', fired === 1,
          fired + ' notifications');
    check('all coalesced writes are present', store.getSnapshot().label === 'b'
          && store.getSnapshot().count === 6 && store.getSnapshot().ratio === 0.2,
          JSON.stringify(store.getSnapshot()));

    // --- a failing method rejects rather than hanging ------------------------
    let rejected = false;
    try { await store.call('no_such_method'); } catch (e) { rejected = true; }
    check('unknown method rejects the promise', rejected);

    // --- unsubscribe --------------------------------------------------------
    unsub();
    const afterUnsub = notifications;
    await store.call('bump', 1);
    await sleep(150);
    check('unsubscribe stops notifications', notifications === afterUnsub,
          notifications + ' vs ' + afterUnsub);

    // --- signals -----------------------------------------------------------
    // A signal-valued property changes INSIDE the signal, so the property-slot
    // observer never fires for it; the store must observe the signal itself.
    // This was silently broken until M3 -- the value simply never updated.
    const SIG = `
import web

var display = clock(freq=10)

type Rig object:
  var temp :signal = signal(10, 0)

var rig = Rig()
rig.temp <- display * 2

web.expose("rig", rig)
display.run()
web.serve(6)
`;
    const sigDone = submit(m, SIG, 'sig.rox');
    await waitFor(() => m.roxalStoreNames().includes('rig'), 30000, 'the signal store');
    const rig = m.roxalStore('rig');

    let sigUpdates = 0;
    const unsubSig = rig.subscribe(() => sigUpdates++);
    await sleep(500);
    const startVal = rig.getSnapshot().temp;
    sigUpdates = 0;
    const t0 = Date.now();
    await sleep(3000);
    const elapsed = (Date.now() - t0) / 1000;
    const rate = sigUpdates / elapsed;
    unsubSig();

    check('signal property reaches the store at all', rig.getSnapshot().temp !== startVal,
          startVal + ' -> ' + rig.getSnapshot().temp);
    // The measured figure is 10.0/s; allow slack for scheduler jitter but stay
    // tight enough to catch "not throttled at all" (which measured ~112/s).
    check('clock-driven signal updates at its clock rate (10 Hz)', rate > 6 && rate < 16,
          rate.toFixed(1) + '/s');
    await sigDone().catch(() => {});

    m.ccall('roxal_quit', null, [], []);
    await done().catch(() => {});

    if (err.trim()) console.log('--- stderr ---\n' + err.trim() + '\n');
    let failed = 0;
    for (const [name, ok, detail] of results) {
        if (!ok) failed++;
        console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   (' + detail + ')' : ''}`);
    }
    console.log(failed ? `\n${failed}/${results.length} FAILED` : `\nall ${results.length} web checks passed`);
    process.exit(failed ? 1 : 0);
})().catch(e => {
    console.error('FAILED: ' + (e && e.stack || e));
    if (err.trim()) console.error('--- stderr ---\n' + err.trim());
    process.exit(1);
});
