// Tests for the web debugger surface: the `debug` store exposed by
// modules/web.rox over an armed session.
//
//   cd build-wasm-mt/dist && node ../../wasm/test-debug.cjs
//
// The contract under test: with roxal_debug_session(1) armed, a script that
// imports web gets the debugger services at import time; breakpoints set by
// display name bind and fire; while STOPPED the debug store stays fully
// serviceable (the excluded Debug actor + the pumping park), inspection and
// evaluation work, and resume completes the program.

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
    for (;;) {
        const v = await pred();
        if (v) return v;
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(25);
    }
}

// Line numbers matter: the breakpoint goes on `total = total + i` (line 6).
const DEBUGGEE = `import web

func work(n :int) -> int:
  var total = 0
  for i in range(..<n):
    total = total + i
    wait(ms=15)
  return total

var r = work(400)
print('r=' + string(r))

# The debugger control store is the HOST's: a script must not be able to
# take the name over and impersonate it.
type Probe object:
  var v :int = 0

try:
  web.expose('debug', Probe())
  print('reserved=taken')
except e:
  print('reserved=refused')
`;

(async () => {
    const createRoxal = require(path.resolve(process.cwd(), 'roxal.js'));
    const m = await createRoxal(Module);

    // Arm the session BEFORE the run: import-time exposure depends on it.
    m.ccall('roxal_debug_session', null, ['number'], [1]);

    const done = submit(m, DEBUGGEE, 'dbg.rox');

    await waitFor(async () => m.roxalStoreNames().includes('debug'),
                  30000, 'the debug store (import-time exposure)');
    const dbg = m.roxalStore('debug');
    check('debug store exposed at import time', true);

    const armed = await dbg.call('arm', true);
    check('arm resolves', armed && armed.ok === true, JSON.stringify(armed));

    // Bind by display name while the program is RUNNING (live edit).
    const bp = await dbg.call('set_breakpoints', 'dbg.rox', [6]);
    check('breakpoint binds by display name',
          bp && bp.ok && bp.breakpoints.length === 1
          && bp.breakpoints[0].verified === true && bp.breakpoints[0].line === 6,
          JSON.stringify(bp));

    // The breakpoint fires; state() reports the stop.  Every one of these
    // store calls is serviced WHILE THE DEBUGGEE IS STOPPED -- the whole
    // point of the excluded actor + pumping park.
    const st = await waitFor(async () => {
        const s = await dbg.call('state');
        return s && s.stopped ? s : null;
    }, 30000, 'the breakpoint stop');
    check('stopped at the breakpoint', st.reason === 'breakpoint' && st.line === 6,
          JSON.stringify(st));

    const threads = await dbg.call('threads');
    check('threads listed while stopped',
          threads.ok && threads.threads.some(t => t.id === st.thread_id),
          JSON.stringify(threads));

    const stack = await dbg.call('stack', st.thread_id);
    check('stack shows the work frame',
          stack.ok && stack.frames.length >= 2
          && stack.frames[0].name === 'work' && stack.frames[0].line === 6,
          JSON.stringify(stack.frames && stack.frames[0]));

    const scopes = await dbg.call('scopes', stack.frames[0].id);
    const locals = scopes.ok && scopes.scopes.find(s => s.name === 'Locals');
    check('locals scope present', !!locals, JSON.stringify(scopes));

    const vars = await dbg.call('variables', locals.ref, 0, 0);
    const byName = {};
    if (vars.ok) for (const v of vars.variables) byName[v.name] = v;
    check('locals include n=400', byName.n && byName.n.value === '400',
          JSON.stringify(byName.n));
    check('synthetic loop temps hidden',
          vars.ok && !vars.variables.some(v => v.name.startsWith('__')),
          JSON.stringify(vars.variables && vars.variables.map(v => v.name)));

    const ev = await dbg.call('evaluate', stack.frames[0].id, 'total');
    check('evaluate resolves a local', ev.ok === true && /^\d+$/.test(ev.value),
          JSON.stringify(ev));

    // Step: land on the NEXT statement, still fully serviceable.
    const stepped = await dbg.call('step', st.thread_id, 'next');
    check('step accepted', stepped.ok === true, JSON.stringify(stepped));
    const st2 = await waitFor(async () => {
        const s = await dbg.call('state');
        return s && s.stopped && s.reason === 'step' ? s : null;
    }, 30000, 'the step stop');
    check('step stop reported', st2.line >= 5 && st2.line <= 7, JSON.stringify(st2));

    // Clear the breakpoint and continue: the program must complete.
    const cleared = await dbg.call('set_breakpoints', 'dbg.rox', []);
    check('breakpoints cleared', cleared.ok === true, JSON.stringify(cleared));
    const resumed = await dbg.call('resume');
    check('resume accepted', resumed.ok === true, JSON.stringify(resumed));

    const rc = await done();
    check('program completed after resume', rc === 0, 'rc=' + rc);
    check('program output arrived', out.includes('r=79800'), JSON.stringify(out));
    check("the 'debug' store name is reserved from scripts",
          out.includes('reserved=refused'), JSON.stringify(out));

    // Breakpoints armed BEFORE the source is submitted: the request table is
    // process-wide and outlives a run, so a host can configure a program it
    // has not started yet.  This is the only way to stop on a line that runs
    // before any store call could reach the debugger.
    {
        const SRC_EARLY = `import web

var seen = 0
for i in range(..<50):
  seen = seen + 1
  wait(ms=10)
print('seen=' + string(seen))
`;
        m.ccall('roxal_debug_arm', null, ['number'], [1]);
        m.ccall('roxal_debug_set_breakpoints', null, ['string', 'string'],
                ['early.rox', '5']);
        const genBefore = m.roxalStoreGeneration('debug');
        const doneEarly = submit(m, SRC_EARLY, 'early.rox');
        await waitFor(async () => m.roxalStoreGeneration('debug') > genBefore,
                      30000, 're-exposed debug store');
        const dbgE = m.roxalStore('debug');
        const st = await waitFor(async () => {
            const v = await dbgE.call('state');
            return v && v.stopped ? v : null;
        }, 30000, 'the pre-armed breakpoint stop');
        check('a breakpoint armed before the run stops on its line',
              st.reason === 'breakpoint' && st.line === 5, JSON.stringify(st));
        // The FIRST iteration, before its assignment runs: seen is still 0.
        // On any later hit it would already be counting.
        const stk = await dbgE.call('stack', st.thread_id);
        const ev = await dbgE.call('evaluate', stk.frames[0].id, 'seen');
        check('the pre-armed stop is the first hit, not a later one',
              ev.ok && ev.value === '0', JSON.stringify(ev));
        await dbgE.call('set_breakpoints', 'early.rox', []);
        await dbgE.call('resume');
        const rc = await doneEarly();
        check('the pre-armed program completes after resume',
              rc === 0 && out.includes('seen=50'), 'rc=' + rc);
    }

    // --- Stopped-state control-surface scenarios --------------------------
    // Pause with a live ordinary actor; the Debug control actor must be
    // absent from threads(); the selected stack must be STABLE across
    // reads; and application code must NOT be able to self-exclude.
    {
        const SRC2 = `import web
import debug

type App actor:
  private var n :int = 0
  proc pump():
    for i in range(..<2000):
      n = n + 1
      wait(ms=10)

var evil = 'unset'
try:
  debug.exclude()
  evil = 'allowed'
except e:
  evil = 'refused'

var a = App()
a.pump()

var beat = 0
for i in range(..<2000):
  beat = beat + 1
  wait(ms=10)
print('beat=' + string(beat))
`;
        // A fresh run constructs a FRESH Debug control actor: wait for the
        // store's re-registration (generation bump) and re-acquire it.
        const genBefore = m.roxalStoreGeneration('debug');
        const done2 = submit(m, SRC2, 'live.rox');
        await waitFor(async () => m.roxalStoreGeneration('debug') > genBefore,
                      30000, 're-exposed debug store');
        const dbg2 = m.roxalStore('debug');
        await sleep(700);
        const st = await waitFor(async () => {
            const p = await dbg2.call('pause');
            if (!p.ok) return null;
            const sst = await dbg2.call('state');
            return sst && sst.stopped ? sst : null;
        }, 20000, 'pause with a live actor');
        check('pause stops with a live ordinary actor', st.reason === 'pause',
              JSON.stringify(st));
        check('pause reports a frozen member thread', st.thread_id > 0,
              JSON.stringify(st.thread_id));

        const th = await dbg2.call('threads');
        check('threads listed on pause', th.ok && th.threads.length >= 1,
              JSON.stringify(th));
        // The Debug control actor keeps RUNNING (it just serviced these
        // calls) -- it must not appear as a frozen inspectable thread.
        // Heuristic-free check: every listed thread must have a readable
        // STABLE stack.
        let stableOk = true;
        let framedCount = 0;
        for (const t of th.threads) {
            const s1 = await dbg2.call('stack', t.id);
            if (!s1.ok) { stableOk = false; continue; }
            if ((s1.frames || []).length === 0) continue;  // engine idles frameless
            framedCount++;
            await sleep(150);
            const s2 = await dbg2.call('stack', t.id);
            const n1 = s1.frames.map(f => f.name + ':' + f.line).join('|');
            const n2 = (s2.frames || []).map(f => f.name + ':' + f.line).join('|');
            if (!s2.ok || n1 !== n2) stableOk = false;
        }
        check('every framed stack is stable across reads', stableOk && framedCount >= 2,
              'framed=' + framedCount);

        // The application-level exclusion attempt must have been refused --
        // `evil` is a module var, visible from the selected thread's frame.
        const stck = await dbg2.call('stack', st.thread_id);
        check('selected thread has frames', stck.ok && stck.frames.length >= 1,
              JSON.stringify(stck));
        const ev = await dbg2.call('evaluate', stck.frames[0].id, 'evil');
        check('application self-exclusion refused',
              ev.ok && ev.value === "'refused'", JSON.stringify(ev));

        await dbg2.call('resume');
        const st3 = await dbg2.call('state');
        check('resumed after live-actor pause', !st3.stopped, JSON.stringify(st3));
        // Don't wait out its 20s of beats: displace it, so the next scenario
        // starts on an idle VM rather than queueing behind it.
        m.ccall('roxal_interrupt_script', null, [], []);
        await done2();
    }

    // A breakpoint request outlives the session that made it.  With no
    // session, a program has no controller to release a stop -- and a parked
    // debuggee answers nothing, not even a request to exit -- so the request
    // must be INERT rather than fatal: the program runs to completion.
    m.ccall('roxal_debug_session', null, ['number'], [0]);
    {
        const SRC3 = `import web

var n = 0
for i in range(..<20):
  n = n + 1
print('n=' + string(n))
`;
        // Arm the request the way the host does, with no session live.
        m.ccall('roxal_debug_set_breakpoints', null, ['string', 'string'],
                ['stale.rox', '4']);
        const done3 = submit(m, SRC3, 'stale.rox');
        const rc = await done3();
        check('a breakpoint with no session does not freeze the program',
              rc === 0 && out.includes('n=20'), 'rc=' + rc);
    }

    m.ccall('roxal_quit', null, [], []);

    let failed = 0;
    for (const [name, ok, detail] of results) {
        console.log(`Test: ${name} ${ok ? 'passed' : 'FAILED'}${ok ? '' : ' - ' + detail}`);
        if (!ok) failed++;
    }
    console.log(`Web debug tests: Passed ${results.length - failed} failed ${failed}`);
    if (err.trim()) console.log('[stderr]', err.trim().slice(0, 500));
    process.exit(failed ? 1 : 0);
})().catch(e => { console.error('harness error:', e); process.exit(2); });
