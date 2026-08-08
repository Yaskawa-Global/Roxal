import { useEffect, useMemo, useRef, useState } from 'react';
import { startRoxal, runScript, scriptParked, ensureServices } from './roxal.js';
import Editor, { disposeModel } from './Editor.jsx';
import { useRoxal, useRoxalStore } from './roxal-react.js';
import APP_SRC from './app.rox.js';

// Files live under /data in the wasm FS, which the host backs with OPFS in the
// browser -- they survive a reload. The IDE reaches them through the
// "workspace" store (a Roxal actor in the web module using fileio itself), so
// File > Open here exercises the same API user scripts get.
const DATA_DIR = '/data';
const BOOTSTRAP = 'import web\nweb.serve()\n';
const LAST_FILE_KEY = 'roxal-ide-last-file';

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

    async function openFile(ws, name) {
        if (!(name in seed)) {
            const text = await ws.call('fs_read', filePath(name));
            setSeed(s => ({ ...s, [name]: text ?? '' }));
        }
        setTabs(t => (t.includes(name) ? t : [...t, name]));
        setActive(name);
        localStorage.setItem(LAST_FILE_KEY, name);
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
                if (!list.includes('app.rox')) {
                    await ws.call('fs_write', filePath('app.rox'), APP_SRC);
                    list = await refreshFiles(ws);
                }
                const last = localStorage.getItem(LAST_FILE_KEY);
                const first = (last && list.includes(last)) ? last : 'app.rox';
                await openFile(ws, first);

                const text = await ws.call('fs_read', filePath(first));
                await runScript(rox, text ?? '', { expectStore: 'workspace' });
                setGeneration(g => g + 1);
            } catch (e) {
                setError(String(e.message || e));
            }
        })();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    async function rerun() {
        if (!workspace || !active) return;
        setRunning(true);
        setRunError(null);
        setRunBenign(false);
        busyRef.current = true;
        try {
            // Save is a store call, so the services have to be alive before it --
            // otherwise Run wedges on its own first step when the previous script
            // has ended.
            await ensureServices(rox, BOOTSTRAP);
            await saveFile(workspace, active);        // Run implies Save
            await runScript(rox, modelText(active), { expectStore: 'workspace' });
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
                await openFile(workspace, action.name);
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
                                { expectStore: 'workspace', assumeStopped: !scriptParked(rox) });
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
                    A Roxal <b>signal network</b> drives the app; files persist in the browser's
                    origin-private file system via Roxal's own <code>fileio</code>; the console
                    evaluates through the compiler. React renders it all
                    through <code>useSyncExternalStore</code> and knows nothing else.
                </p>
            </header>

            <div className="workbench">
                {/* left: the source */}
                <section className="pane editor-pane">
                    <div className="pane-head">
                        <FileMenu files={files} onAction={onMenu} />
                        <div className="tabs">
                            {tabs.map(name => (
                                <span key={name}
                                      className={'tab' + (name === active ? ' active' : '')}
                                      onClick={() => { setActive(name); localStorage.setItem(LAST_FILE_KEY, name); }}>
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
                        <div className="pane-head"><h2>app</h2></div>
                        <div className="pane-body">
                            {error && <pre className="error">{error}</pre>}
                            {!rox && !error && <p className="loading">starting the Roxal VM…</p>}
                            {rox && <OvenPanel key={generation} rox={rox} />}
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
