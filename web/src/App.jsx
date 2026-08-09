import { useEffect, useMemo, useRef, useState } from 'react';
import { startRoxal, runScript, scriptParked, ensureServices } from './roxal.js';
import { warmNnProvider } from './nn-provider.js';
import Editor, { disposeModel } from './Editor.jsx';
import TanksPanel from './TanksPanel.jsx';
import MnistPanel from './MnistPanel.jsx';
import { lazy, Suspense } from 'react';
const Tanks3D = lazy(() => import('./Tanks3D.jsx'));
import { useRoxal, useRoxalStore } from './roxal-react.js';
import OVEN_SRC from './oven.rox.js';
import TANKS_SRC from './tanks.rox.js';
import MNIST_SRC from './mnist.rox.js';

// The examples the IDE seeds on a first visit. All are ordinary files
// afterwards: edit, save, re-run, or open another from the File menu.
// tanks.rox opens first for a visitor with no history -- it is the one that
// shows what a signal network looks like at a glance. mnist.rox is a batch
// script (prints and ends) demonstrating in-browser ai.nn inference.
const EXAMPLES = { 'tanks.rox': TANKS_SRC, 'oven.rox': OVEN_SRC, 'mnist.rox': MNIST_SRC };
// Which panel the app pane shows is decided by WHICH STORE the running script
// exposed -- and by generation, not mere presence, because the JS registry
// keeps a stale record for every store any previous run exposed.
const PANELS = ['tanks', 'oven', 'digit'];
const pickPanel = rox =>
    PANELS.map(n => ({ n, g: rox.roxalStoreGeneration?.(n) ?? 0 }))
          .filter(x => x.g > 0)
          .sort((a, b) => b.g - a.g)[0]?.n ?? null;

// Files live under /data in the wasm FS, which the host backs with OPFS in the
// browser -- they survive a reload. The IDE reaches them through the
// "workspace" store (a Roxal actor in the web module using fileio itself), so
// File > Open here exercises the same API user scripts get.
const DATA_DIR = '/data';
const BOOTSTRAP = 'import web\nweb.serve()\n';
const LAST_FILE_KEY = 'roxal-ide-last-file';
const SEED_STAMP_KEY = 'roxal-ide-seeded';

// FNV-1a, enough to answer "is this byte-for-byte what we wrote?" -- the only
// question asked of it. Storing the full text would work too and cost 8KB of
// localStorage per deploy's worth of examples.
function hash(text) {
    let h = 0x811c9dc5;
    for (let i = 0; i < text.length; i++) {
        h ^= text.charCodeAt(i);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h.toString(36);
}
// Copies we shipped in an earlier deploy, by hash. A visitor from before the
// stamping above has no record, so a pristine old copy is indistinguishable
// from an edited one -- except when it matches a version we know we wrote.
// Add the outgoing hash here whenever an example changes.
const KNOWN_PRIOR = {
    'mnist.rox': ['11ljg1w'],   // the print-and-exit version, pre drawing panel
};
const readStamps = () => {
    try { return JSON.parse(localStorage.getItem(SEED_STAMP_KEY) || '{}'); }
    catch { return {}; }
};
const writeStamps = s => {
    try { localStorage.setItem(SEED_STAMP_KEY, JSON.stringify(s)); } catch { /* private mode */ }
};
const SOURCE_OPEN_KEY = 'roxal-ide-source-open';
const TANKS_VIEW_KEY = 'roxal-ide-tanks-view';

// Everything below is ordinary React. The only Roxal-aware lines are the two
// hooks -- useRoxal for state, useRoxalStore for actions. No effects to wire, no
// polling, no message plumbing: the store is an external store and React already
// knows how to consume one.
function OvenPanel({ rox }) {
    const oven = useRoxal(rox, 'oven');
    const store = useRoxalStore(rox, 'oven');

    if (oven.temperature === undefined)
        return <p className="loading">this app exposes no "oven" store — watch the output pane</p>;

    return (
        <div className="panel">
            <div className="readout">
                <Field label="temperature" value={fmt(oven.temperature, 1) + '°'} big />
                <Field label="setpoint" value={fmt(oven.setpoint, 0) + '°'} />
                <Field label="error" value={fmt(oven.error, 1)} />
                <Field label="heater" value={oven.heating ? 'ON' : 'off'}
                       tone={oven.heating ? 'busy' : 'calm'} />
            </div>

            <Track position={(oven.temperature ?? 0) / 3} target={(oven.setpoint ?? 0) / 3} />
            <Duty value={oven.duty ?? 0} />

            <label className="slider">
                <span>setpoint</span>
                {/* Writes straight into the Roxal signal -- the network picks it up. */}
                <input type="range" min="20" max="260" step="5"
                       value={oven.setpoint ?? 20}
                       onChange={e => store.set('setpoint', Number(e.target.value))} />
                <b>{fmt(oven.setpoint, 0)}°</b>
            </label>

            <div className="actions">
                <button onClick={() => store.call('preheat')}>preheat 150°</button>
                <button onClick={() => store.call('reflow')}>reflow 240°</button>
                <button onClick={() => store.call('cool')}>cool</button>
            </div>

            <p className="log">{oven.phase}</p>
        </div>
    );
}

const fmt = (v, places) => (typeof v === 'number' ? v.toFixed(places) : '–');

function Field({ label, value, big, tone }) {
    return (
        <div className="field">
            <span className="k">{label}</span>
            <span className={'v' + (big ? ' big' : '') + (tone ? ' ' + tone : '')}>
                {value ?? '–'}
            </span>
        </div>
    );
}

// The lifted duty_for() node, rendered as a bar.
function Duty({ value }) {
    return (
        <div className="duty">
            <span className="k">duty</span>
            <div className="duty-track">
                <div className="duty-fill" style={{ width: Math.round(value * 100) + '%' }} />
            </div>
        </div>
    );
}

// A view that only makes sense because temperature arrives at a steady 20 Hz.
function Track({ position, target }) {
    const pct = v => Math.max(0, Math.min(100, v));
    return (
        <div className="track">
            <div className="track-target" style={{ left: pct(target) + '%' }} />
            <div className="track-pos" style={{ left: pct(position) + '%' }} />
        </div>
    );
}

// A zero-JS dropdown (details/summary); items close it by blurring the details.
function FileMenu({ files, onAction }) {
    const ref = useRef(null);
    const pick = action => () => { ref.current?.removeAttribute('open'); onAction(action); };
    return (
        <details className="menu" ref={ref}>
            <summary>File</summary>
            <div className="menu-items">
                <button onClick={pick({ kind: 'new' })}>New…</button>
                <div className="menu-sep" />
                {files.length === 0 && <span className="menu-note">(no files)</span>}
                {files.map(f => (
                    <button key={f} onClick={pick({ kind: 'open', name: f })}>{f}</button>
                ))}
                <div className="menu-sep" />
                <button onClick={pick({ kind: 'save' })}>Save</button>
                <button onClick={pick({ kind: 'saveAs' })}>Save As…</button>
                <button onClick={pick({ kind: 'delete' })}>Delete</button>
            </div>
        </details>
    );
}

// REPL: each line goes to workspace.eval() on the VM; expression results and
// prints come back over stdout, so the entry captures the output delta.
function Repl({ evalLine }) {
    const [log, setLog] = useState([]);
    const [input, setInput] = useState('');
    const [history, setHistory] = useState([]);
    const [histAt, setHistAt] = useState(-1);
    const endRef = useRef(null);

    useEffect(() => { endRef.current?.scrollIntoView({ block: 'nearest' }); }, [log]);

    async function submit() {
        const line = input.trim();
        if (!line) return;
        setInput('');
        setHistory(h => [line, ...h]);
        setHistAt(-1);
        const out = await evalLine(line);
        setLog(l => [...l, { line, out }]);
    }

    function key(e) {
        if (e.key === 'Enter') { e.preventDefault(); submit(); }
        else if (e.key === 'ArrowUp') {
            e.preventDefault();
            const at = Math.min(histAt + 1, history.length - 1);
            if (at >= 0 && history[at] !== undefined) { setHistAt(at); setInput(history[at]); }
        } else if (e.key === 'ArrowDown') {
            e.preventDefault();
            const at = histAt - 1;
            setHistAt(at);
            setInput(at >= 0 ? history[at] : '');
        }
    }

    return (
        <div className="repl">
            <div className="repl-log">
                {log.map((e, i) => (
                    <div key={i}>
                        <div className="repl-in">&gt; {e.line}</div>
                        {e.out && <div className="repl-out">{e.out}</div>}
                    </div>
                ))}
                <div ref={endRef} />
            </div>
            <input className="repl-input" placeholder="roxal expression — try 6 * 7"
                   value={input}
                   onChange={e => setInput(e.target.value)}
                   onKeyDown={key} />
        </div>
    );
}

export default function App() {
    const [rox, setRox] = useState(null);
    const [error, setError] = useState(null);
    const [output, setOutput] = useState('');
    const [running, setRunning] = useState(false);
    const [runError, setRunError] = useState(null);
    // A finished batch script is not a failure -- it just has no live app.
    const [runBenign, setRunBenign] = useState(false);

    // Workspace state. Monaco models are the source of truth for content; this
    // component tracks which files exist, which are open, and which is active.
    const [files, setFiles] = useState([]);          // names in /data
    const [tabs, setTabs] = useState([]);            // open file names
    const [active, setActive] = useState(null);      // active file name
    const [seed, setSeed] = useState({});            // name -> initial content
    const [dirtyTabs, setDirtyTabs] = useState({});  // name -> bool

    // Stable store handles (roxalStore returns a fresh object per call).
    const ide = useMemo(() => (rox ? rox.roxalStore('ide') : null), [rox]);
    const workspace = useMemo(() => (rox ? rox.roxalStore('workspace') : null), [rox]);
    // Remount the panel after a re-run so hooks re-read the replaced store.
    const [generation, setGeneration] = useState(0);
    const [panel, setPanel] = useState(null);
    // Collapsing the source pane hands the whole width to the running app --
    // which is what a demo wants, and what a small screen needs. Remembered,
    // because a preference that resets every reload is not a preference.
    const [sourceOpen, setSourceOpen] = useState(
        () => localStorage.getItem(SOURCE_OPEN_KEY) !== 'false');
    const toggleSource = () => setSourceOpen(v => {
        localStorage.setItem(SOURCE_OPEN_KEY, String(!v));
        return !v;
    });
    // 2D or 3D for the tanks app -- same store either way; tanks.rox does not
    // know which view it has, which is rather the point.
    const [tanksView, setTanksView] = useState(
        () => localStorage.getItem(TANKS_VIEW_KEY) || '3d');
    const pickView = v => { localStorage.setItem(TANKS_VIEW_KEY, v); setTanksView(v); };
    // Pauses the RENDERER, not the app: the network keeps running (it is the
    // thing being demonstrated, and it is cheap); the WebGL loop is what spins
    // a CPU/GPU. Deliberately not persisted -- a fresh page should play.
    const [paused, setPaused] = useState(false);
    // Output length marker for REPL capture.
    const outputRef = useRef('');
    // True while THIS component is deliberately stopping/starting a script, so
    // the liveness watchdog does not re-park its own bootstrap underneath a
    // restart already in progress -- two recoveries racing leaves the second
    // one queued behind the first one's parked script, i.e. never running.
    const busyRef = useRef(false);

    const filePath = name => DATA_DIR + '/' + name;
    const modelText = name => {
        const monaco = window.monaco;
        const m = monaco?.editor.getModel(monaco.Uri.parse('inmemory://roxal' + filePath(name)));
        return m ? m.getValue() : (seed[name] ?? '');
    };

    async function refreshFiles(ws) {
        const list = await ws.call('fs_list', DATA_DIR);
        setFiles((list || []).filter(n => !n.endsWith('/')));
        return list || [];
    }

    // Returns the file's text: the caller may want to run it, and React state
    // set here is not readable until the next render, so handing it back is the
    // only way a caller can act on what was just opened.
    async function openFile(ws, name) {
        const monaco = window.monaco;
        // The live editor buffer wins -- it may hold unsaved edits.
        const m = monaco?.editor.getModel(monaco.Uri.parse('inmemory://roxal' + filePath(name)));
        let text = m ? m.getValue() : seed[name];
        if (text === undefined) {
            text = (await ws.call('fs_read', filePath(name))) ?? '';
            setSeed(s => ({ ...s, [name]: text }));
        }
        setTabs(t => (t.includes(name) ? t : [...t, name]));
        setActive(name);
        localStorage.setItem(LAST_FILE_KEY, name);
        return text;
    }

    async function saveFile(ws, name) {
        const ok = await ws.call('fs_write', filePath(name), modelText(name));
        if (ok) setDirtyTabs(d => ({ ...d, [name]: false }));
        return ok;
    }

    // Boot: start a bootstrap script (exposes the workspace), seed /data with
    // the demo on first visit, open the last-used file, then run it.
    useEffect(() => {
        (async () => {
            try {
                const { rox } = await startRoxal(BOOTSTRAP, {
                    expectStore: 'workspace',
                    onOutput: text => { outputRef.current = text; setOutput(text); },
                });
                setRox(rox);
                const ws = rox.roxalStore('workspace');

                let list = await refreshFiles(ws);
                let seeded = false;
                // Seeding an example ONCE is not enough: the files live in OPFS
                // and outlive every deploy, so a shipped example that changes
                // never reaches anyone who already has the old copy -- they see
                // a stale demo and no reason why. Update the copies the user has
                // not touched, and never the ones they have: `stamps` records
                // what we last wrote, so "unchanged since we wrote it" is
                // decidable rather than guessed.
                const stamps = readStamps();
                for (const [name, src] of Object.entries(EXAMPLES)) {
                    if (!list.includes(name)) {
                        await ws.call('fs_write', filePath(name), src);
                        stamps[name] = hash(src);
                        seeded = true;
                        continue;
                    }
                    const current = (await ws.call('fs_read', filePath(name))) ?? '';
                    const now = hash(current), shipped = hash(src), stamp = stamps[name];
                    const update = stamp === undefined
                        // No record of writing it: refresh ONLY a copy we
                        // recognise as an older shipment of our own.
                        ? (KNOWN_PRIOR[name] || []).includes(now)
                        // We wrote it: refresh while it is untouched since.
                        : now === stamp && stamp !== shipped;
                    if (update) {
                        await ws.call('fs_write', filePath(name), src);
                        stamps[name] = shipped;
                        disposeModel(filePath(name));   // drop the editor's stale buffer
                        setSeed(s => ({ ...s, [name]: src }));
                        seeded = true;
                    } else if (stamp === undefined) {
                        // Adopt an unrecognised copy: it may be the user's work,
                        // so record it and never touch it. Deliberately NOT done
                        // when a stamp already exists and no longer matches --
                        // that stamp is the proof the user has edited since.
                        stamps[name] = now;
                    }
                }
                writeStamps(stamps);
                if (seeded) list = await refreshFiles(ws);
                const last = localStorage.getItem(LAST_FILE_KEY);
                const first = (last && list.includes(last)) ? last : 'tanks.rox';
                await openFile(ws, first);

                const text = await ws.call('fs_read', filePath(first));
                try {
                    await runScript(rox, text ?? '', { expectStore: 'workspace', name: first });
                } catch (e) {
                    // A batch example (mnist.rox) as the remembered file: it ran
                    // and finished, which is success, not a boot failure. Its
                    // prints are in the output pane; put the services back.
                    if (!(e.scriptEnded && e.rc === 0)) throw e;
                    await ensureServices(rox, BOOTSTRAP);
                }
                setPanel(pickPanel(rox));
                setGeneration(g => g + 1);
            } catch (e) {
                setError(String(e.message || e));
            }
        })();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // One path for "make this file the running program", shared by the Run
    // button and by opening a file. `source` exists because a just-opened file
    // is not yet readable from React state; `save` is false for that case,
    // since the text came off disk unchanged.
    async function runNamed(name, { source, save = true } = {}) {
        if (!workspace || !name) return;
        // A script that loads a model will block the VM inside Model() until
        // the runtime is there; start fetching it now so that wait is short.
        if ((source ?? modelText(name)).includes('ai.nn')) warmNnProvider();
        setRunning(true);
        setRunError(null);
        setRunBenign(false);
        busyRef.current = true;
        try {
            // Save is a store call, so the services have to be alive before it --
            // otherwise Run wedges on its own first step when the previous script
            // has ended.
            await ensureServices(rox, BOOTSTRAP);
            if (save) await saveFile(workspace, name);        // Run implies Save
            await runScript(rox, source ?? modelText(name), { expectStore: 'workspace', name });
            setPanel(pickPanel(rox));
            setGeneration(g => g + 1);
        } catch (e) {
            setRunError(String(e.message || e));
            setRunBenign(Boolean(e.scriptEnded) && e.rc === 0);
            // A script that ended (batch script, or an error before serve) left
            // nothing parked. Put the IDE's own services back so the menus,
            // console and Run keep working.
            try { await ensureServices(rox, BOOTSTRAP); } catch { /* reported above */ }
        } finally {
            busyRef.current = false;
            setRunning(false);
        }
    }

    const rerun = () => runNamed(active);

    // Choosing a script -- from the File menu or its tab -- means "show me this
    // one". Opening without running left the app pane displaying the PREVIOUS
    // script's UI, which reads as the selection not having worked (and a reload
    // then "fixed" it, because boot runs whatever file it reopens).
    async function openAndRun(name) {
        const text = await openFile(workspace, name);
        await runNamed(name, { source: text, save: false });
    }

    // Watchdog for the Run button. Every step of a run is individually bounded
    // (store calls time out, the start/stop loops have deadlines), but they are
    // sequential -- a bad run can chain several timeouts and leave the button
    // reading "running…" long enough to look permanently wedged. Nothing here
    // cancels the work; it just stops the UI from lying about it.
    useEffect(() => {
        if (!running) return;
        const id = setTimeout(() => {
            busyRef.current = false;
            setRunning(false);
            setRunBenign(false);
            setRunError('the run is taking unusually long — the VM may be busy '
                      + '(loading a model runtime, or a script that will not stop). '
                      + 'Press Run again, or reload if it persists.');
        }, 45_000);
        return () => clearTimeout(id);
    }, [running]);

    // A script can also end on its own -- a batch script finishing, or a fatal
    // runtime error minutes later. Poll the liveness invariant and restore the
    // services when it breaks, so the IDE never silently goes dead.
    useEffect(() => {
        if (!rox) return;
        const id = setInterval(async () => {
            if (running || busyRef.current || scriptParked(rox)) return;
            try {
                if (await ensureServices(rox, BOOTSTRAP)) setGeneration(g => g + 1);
            } catch { /* next tick tries again */ }
        }, 1000);
        return () => clearInterval(id);
    }, [rox, running]);

    useEffect(() => {
        const onKey = e => {
            if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'b') {
                e.preventDefault();
                toggleSource();
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    async function onMenu(action) {
        if (!workspace) return;
        try {
            await ensureServices(rox, BOOTSTRAP);
            if (action.kind === 'new') {
                let name = window.prompt('New file name', 'untitled.rox');
                if (!name) return;
                if (!name.endsWith('.rox')) name += '.rox';
                await workspace.call('fs_write', filePath(name), '// ' + name + '\n');
                await refreshFiles(workspace);
                await openFile(workspace, name);
            } else if (action.kind === 'open') {
                await openAndRun(action.name);
            } else if (action.kind === 'save' && active) {
                await saveFile(workspace, active);
            } else if (action.kind === 'saveAs' && active) {
                let name = window.prompt('Save as', active);
                if (!name) return;
                if (!name.endsWith('.rox')) name += '.rox';
                await workspace.call('fs_write', filePath(name), modelText(active));
                await refreshFiles(workspace);
                await openFile(workspace, name);
            } else if (action.kind === 'delete' && active) {
                if (!window.confirm('Delete ' + active + '?')) return;
                await workspace.call('fs_delete', filePath(active));
                disposeModel(filePath(active));
                setSeed(({ [active]: _, ...rest }) => rest);
                const remaining = tabs.filter(t => t !== active);
                setTabs(remaining);
                await refreshFiles(workspace);
                if (remaining.length) setActive(remaining[0]);
                else { setActive(null); }
            }
        } catch (e) {
            setRunError(String(e.message || e));
        }
    }

    function closeTab(name) {
        const remaining = tabs.filter(t => t !== name);
        setTabs(remaining);
        if (active === name) setActive(remaining[remaining.length - 1] ?? null);
    }

    // REPL: run the line, return whatever it printed (or the error string).
    //
    // A line can hit a FATAL runtime error (most Roxal runtime errors are, by
    // design) which takes down the whole running app -- and with it the pump
    // that would resolve this very call. So: race a timeout, then check
    // whether the app died and restart it. The fatal message itself arrives
    // on stderr, which is already in the captured output slice.
    async function evalLine(line) {
        if (!workspace || !rox) return '(VM not ready)';
        const before = outputRef.current.length;
        const completedBefore = rox.ccall('roxal_completed_count', 'number', [], []);
        let err = '';
        const result = await Promise.race([
            workspace.call('eval', line).catch(e => String(e.message || e)),
            new Promise(r => setTimeout(() => r('__eval_timeout__'), 5000)),
        ]);
        if (result !== '__eval_timeout__') err = result || '';
        // Give the coalesced output flush a beat to land.
        await new Promise(r => setTimeout(r, 60));
        let note = '';
        if (rox.ccall('roxal_completed_count', 'number', [], []) > completedBefore) {
            // The line killed the app. Bring it back.
            busyRef.current = true;
            try {
                // The watchdog may already have re-parked the bootstrap; if so
                // this has to stop it first, or the app would queue behind it.
                await runScript(rox, modelText(active),
                                { expectStore: 'workspace', assumeStopped: !scriptParked(rox),
                                  name: active });
                setGeneration(g => g + 1);
                note = '\n(fatal error — app restarted)';
            } catch (e2) {
                // The app itself will not come back (it may not even park).
                // Services still must, or the console dies with it.
                try {
                    await ensureServices(rox, BOOTSTRAP);
                    note = '\n(fatal error — app stopped; IDE services restarted)';
                } catch {
                    note = '\n(fatal error — restart failed: ' + String(e2.message || e2) + ')';
                }
            } finally {
                busyRef.current = false;
            }
        } else if (result === '__eval_timeout__') {
            note = '(still running — check the output pane)';
        }
        const printed = outputRef.current.slice(before).replace(/^\[stderr\] /gm, '');
        return (printed + (err || '') + note).trimEnd();
    }

    return (
        <main>
            <header>
                <h1>Roxal + React</h1>
                <p className="sub">
                    A Roxal script drives the app — the File menu switches between
                    a <b>signal network</b> (<code>tanks.rox</code>, <code>oven.rox</code>) and
                    a <b>neural net</b> running in this tab (<code>mnist.rox</code>); files persist
                    in the browser's origin-private file system via Roxal's
                    own <code>fileio</code>; the console evaluates through the compiler. React
                    renders it all through <code>useSyncExternalStore</code> and knows nothing else.
                </p>
            </header>

            <div className={'workbench' + (sourceOpen ? '' : ' source-collapsed')}>
                {/* left: the source */}
                <section className="pane editor-pane">
                    <div className="pane-head">
                        <button className="collapse" onClick={toggleSource}
                                title={(sourceOpen ? 'hide' : 'show') + ' the source (Ctrl/⌘-B)'}>
                            {sourceOpen ? '▾' : '▸'} source
                        </button>
                        <FileMenu files={files} onAction={onMenu} />
                        <div className="tabs">
                            {tabs.map(name => (
                                <span key={name}
                                      className={'tab' + (name === active ? ' active' : '')}
                                      onClick={() => { if (name !== active) openAndRun(name); }}>
                                    {name}{dirtyTabs[name] ? ' •' : ''}
                                    {tabs.length > 1 &&
                                        <b className="tab-close"
                                           onClick={e => { e.stopPropagation(); closeTab(name); }}>×</b>}
                                </span>
                            ))}
                        </div>
                        <button className="run" disabled={!rox || running || !active}
                                onClick={rerun}>{running ? 'running…' : 'Run'}</button>
                        {runError && <span className={runBenign ? 'run-note' : 'run-error'}>{runError}</span>}
                    </div>
                    {active
                        ? <Editor path={filePath(active)}
                                  content={seed[active] ?? ''}
                                  onChange={(path, _text) => {
                                      const name = path.slice(DATA_DIR.length + 1);
                                      setDirtyTabs(d => (d[name] ? d : { ...d, [name]: true }));
                                  }}
                                  service={ide}
                                  height="100%" />
                        : <p className="loading">no file open — File → New…</p>}
                </section>

                {/* right: the running app above, output and console below */}
                <div className="right-column">
                    <section className="pane app-pane">
                        <div className="pane-head">
                            <h2>app</h2>
                            {panel === 'tanks' &&
                                <span className="view-toggle">
                                    {tanksView === '3d' &&
                                        <button className={'collapse' + (paused ? ' active' : '')}
                                                title={paused ? 'resume rendering' : 'pause rendering (the app keeps running)'}
                                                onClick={() => setPaused(p => !p)}>
                                            {paused ? '▶' : '⏸'}
                                        </button>}
                                    {['2d', '3d'].map(v => (
                                        <button key={v}
                                                className={'collapse' + (tanksView === v ? ' active' : '')}
                                                onClick={() => pickView(v)}>{v.toUpperCase()}</button>
                                    ))}
                                </span>}
                        </div>
                        <div className="pane-body">
                            {error && <pre className="error">{error}</pre>}
                            {!rox && !error && <p className="loading">starting the Roxal VM…</p>}
                            {rox && panel === 'tanks' && (tanksView === '3d'
                                ? <Suspense fallback={<p className="loading">loading the 3D view…</p>}>
                                      <Tanks3D key={generation} rox={rox} paused={paused} />
                                  </Suspense>
                                : <TanksPanel key={generation} rox={rox} />)}
                            {rox && panel === 'oven' && <OvenPanel key={generation} rox={rox} />}
                            {rox && panel === 'digit' && <MnistPanel key={generation} rox={rox} />}
                            {rox && !panel && !error &&
                                <p className="loading">the running script exposes no known app store</p>}
                        </div>
                    </section>

                    <section className="pane output-pane">
                        <div className="pane-head"><h2>output</h2></div>
                        <pre className="pane-body out">{output || '(nothing yet)'}</pre>
                    </section>

                    <section className="pane repl-pane">
                        <div className="pane-head"><h2>console</h2></div>
                        <Repl evalLine={evalLine} />
                    </section>
                </div>
            </div>
        </main>
    );
}
