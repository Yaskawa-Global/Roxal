import { lazy, Suspense, useEffect, useMemo, useRef, useState } from 'react';
import { startRoxal, runScript, scriptParked, ensureServices, stopCurrent } from '../roxal.js';
import { hostUrlFromLocation } from '../lib/host.js';
import Editor, { disposeModel } from '../Editor.jsx';
import FileMenu from '../lib/FileMenu.jsx';
import { askText } from '../lib/prompt.js';
import Repl from '../lib/Repl.jsx';
import { isDiagramSource, dfCall } from '../dfdoc.js';

// AI Studio: the data-flow editor as the whole window, driving a NATIVE Roxal
// VM (`roxal --web-host`) so diagrams can hold ONNX model nodes on the GPU,
// OpenCV nodes, and camera feeds. The canvas is the main view; the source is
// a tab; the console is a drawer, closed by default. No debugger.
//
// Everything Roxal-facing is the same machinery the IDE uses: the workspace
// and df stores the web module exposes, the harness a diagram runs as, the
// liveness invariant that keeps services parked. Only the shell differs.

const DfEditor = lazy(() => import('../DfEditor.jsx'));

// Where the native host listens unless ?host= says otherwise.
const DEFAULT_HOST = 'ws://127.0.0.1:8765';

// The bootstrap parks the page's services (workspace files, the df document
// service, the REPL) and reports what this host is.
const BOOTSTRAP = [
    'import web',
    'type Build object:',
    '  var platform :string = platform',
    '  var version :string = version',
    '  var features = features',
    '  var data_dir :string = host_dir("data")',
    'web.expose("build", Build())',
    'web.serve()',
    '',
].join('\n');

const LAST_FILE_KEY = 'aistudio-last-file';
// Inside the Electron shell (the desktop app) edits are NOT saved on their
// own: the file keeps its unsaved marker until File > Save (Ctrl+S), and a
// run uses the buffer without touching the file. The web app keeps its
// autosave, where the browser tab is the less durable side.
const DESKTOP = Boolean(window.aistudio);
const DRAWER_KEY = 'aistudio-drawer';

// Palette base for AI diagrams: the vision/model modules land here as they
// arrive (M4/M5); the document's own imports always add theirs.
const PALETTE_MODULES = ['math', 'logic'];

export default function App() {
    const [rox, setRox] = useState(null);
    const roxRef = useRef(null);
    const [error, setError] = useState(null);
    const [build, setBuild] = useState(null);          // {version, features, data_dir}
    const [dataDir, setDataDir] = useState('/data');
    const dataDirRef = useRef('/data');
    const [output, setOutput] = useState('');
    const outputRef = useRef('');
    const [running, setRunning] = useState(false);
    const [live, setLive] = useState(false);           // a diagram harness is parked
    const [runError, setRunError] = useState(null);
    const [runBenign, setRunBenign] = useState(false);

    const [files, setFiles] = useState([]);
    const [active, setActive] = useState(null);
    const [seed, setSeed] = useState({});               // name -> text (until Monaco holds it)
    const [dirty, setDirty] = useState({});
    const [viewMode, setViewMode] = useState('diagram');
    const [drawerOpen, setDrawerOpen] = useState(() => localStorage.getItem(DRAWER_KEY) === 'open');
    const [outputClearAt, setOutputClearAt] = useState(0);
    const [replGen, setReplGen] = useState(0);
    const [generation, setGeneration] = useState(0);

    const workspace = useMemo(() => (rox ? rox.roxalStore('workspace') : null), [rox]);
    const ide = useMemo(() => (rox ? rox.roxalStore('ide') : null), [rox]);
    const df = useMemo(() => (rox ? rox.roxalStore('df') : null), [rox]);

    const busyRef = useRef(false);
    const runSeqRef = useRef(0);
    const runChainRef = useRef(Promise.resolve());
    const autosaveRef = useRef(null);
    const outPaneRef = useRef(null);
    const outPinnedRef = useRef(true);

    useEffect(() => { roxRef.current = rox; window.__rox = rox; }, [rox]);
    useEffect(() => () => clearTimeout(autosaveRef.current), []);
    useEffect(() => {
        const el = outPaneRef.current;
        if (el && outPinnedRef.current) el.scrollTop = el.scrollHeight;
    }, [output, outputClearAt, drawerOpen]);

    const filePath = name => dataDirRef.current + '/' + name;
    // The seed (a file's text before Monaco has a model for it) is read
    // through a ref: a save scheduled by an earlier render must see the text
    // written since, not the state that render closed over.
    const seedRef = useRef(seed);
    seedRef.current = seed;
    const modelText = name => {
        const monaco = window.monaco;
        const m = monaco?.editor.getModel(monaco.Uri.parse('inmemory://roxal' + filePath(name)));
        return m ? m.getValue() : (seedRef.current[name] ?? '');
    };
    const writeModel = (name, src) => {
        const monaco = window.monaco;
        const m = monaco?.editor.getModel(monaco.Uri.parse('inmemory://roxal' + filePath(name)));
        if (m) { if (m.getValue() !== src) m.setValue(src); }
        else setSeed(s => (s[name] === src ? s : { ...s, [name]: src }));
    };
    const activeIsDiagram = active != null && isDiagramSource(modelText(active));
    const activeView = activeIsDiagram ? viewMode : 'source';

    async function refreshFiles(ws) {
        const list = await ws.call('fs_list', dataDirRef.current);
        const names = (list || []).filter(n => !n.endsWith('/') && !n.startsWith('.') && n.endsWith('.rox'));
        setFiles(names);
        return names;
    }

    async function openFile(ws, name) {
        const monaco = window.monaco;
        const m = monaco?.editor.getModel(monaco.Uri.parse('inmemory://roxal' + filePath(name)));
        let text = m ? m.getValue() : seed[name];
        if (text === undefined) {
            text = (await ws.call('fs_read', filePath(name))) ?? '';
            setSeed(s => ({ ...s, [name]: text }));
        }
        setActive(name);
        setViewMode('diagram');
        localStorage.setItem(LAST_FILE_KEY, name);
        return text;
    }

    async function saveFile(ws, name) {
        const ok = await ws.call('fs_write', filePath(name), modelText(name));
        if (ok) setDirty(d => ({ ...d, [name]: false }));
        return ok;
    }

    async function ensureServicesQuiet() {
        const vm = roxRef.current;
        if (!vm) return false;
        return ensureServices(vm, BOOTSTRAP);
    }

    // Boot: connect to the host, learn where its files are, open the last
    // (or first) diagram and run it.
    useEffect(() => {
        (async () => {
            try {
                const hostUrl = hostUrlFromLocation() ?? DEFAULT_HOST;
                const { rox } = await startRoxal(BOOTSTRAP, {
                    expectStore: 'workspace',
                    onOutput: text => { outputRef.current = text; setOutput(text); },
                    hostUrl,
                });
                roxRef.current = rox;
                setRox(rox);
                const b = rox.roxalStore('build').getSnapshot();
                setBuild(b);
                if (typeof b.data_dir === 'string' && b.data_dir) {
                    dataDirRef.current = b.data_dir;
                    setDataDir(b.data_dir);
                }
                const ws = rox.roxalStore('workspace');
                const list = await refreshFiles(ws);
                const wanted = (new URLSearchParams(location.search).get('file') || '').trim();
                const last = localStorage.getItem(LAST_FILE_KEY);
                const first = [wanted, wanted ? wanted + '.rox' : null, last]
                    .find(n => n && list.includes(n)) ?? list[0];
                if (first) {
                    const text = await openFile(ws, first);
                    await runNamed(first, { source: text, save: false });
                } else {
                    await ensureServicesQuiet();
                }
            } catch (e) {
                setError(String(e.message || e));
            }
        })();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    function runNamed(name, opts = {}) {
        const seq = ++runSeqRef.current;
        const p = runChainRef.current.catch(() => {}).then(() => startRun(name, seq, opts));
        runChainRef.current = p.catch(() => {});
        return p;
    }

    async function startRun(name, seq, { source, save = true } = {}) {
        const vm = roxRef.current;
        const superseded = () => runSeqRef.current !== seq;
        if (superseded()) return;
        if (!vm || !name) { busyRef.current = false; setRunning(false); return; }
        const ws = vm.roxalStore('workspace');
        const dfs = vm.roxalStore('df');
        setRunning(true);
        setRunError(null);
        setRunBenign(false);
        busyRef.current = true;
        try {
            await stopCurrent(vm);
            if (superseded()) return;
            await ensureServicesQuiet();
            if (superseded()) return;
            if (save && !DESKTOP) await saveFile(ws, name);
            if (superseded()) return;
            let text = source ?? modelText(name);
            let isDiagram = false;
            if (isDiagramSource(text)) {
                // A diagram defines a component; running it means running the
                // generated harness, which instantiates it, attaches the
                // probes and parks. Check first: an unconnected port would
                // only fail deep inside wiring.
                isDiagram = true;
                const p = filePath(name);
                let opened = await dfCall(dfs, 'save', p);
                if (!opened.ok && opened.error === 'no_document')
                    opened = await dfCall(dfs, 'open', p, text);
                if (!opened.ok) throw new Error('diagram: ' + opened.error);
                const chk = await dfCall(dfs, 'check', p);
                const errs = (chk.ok ? chk.diagnostics : []).filter(d => d.level === 'error');
                if (errs.length)
                    throw new Error('diagram has errors — ' + errs.map(d => d.message).join('; '));
                const h = await dfCall(dfs, 'harness', p);
                if (!h.ok) throw new Error('diagram harness: ' + h.error);
                text = h.source;
            }
            await runScript(vm, text, { expectStore: 'workspace',
                                        assumeStopped: !scriptParked(vm),
                                        name: isDiagram ? filePath(name) : name,
                                        superseded });
            setLive(true);
            setGeneration(g => g + 1);
        } catch (e) {
            if (e && e.superseded) return;
            const msg = String(e.message || e);
            setRunError(msg);
            setRunBenign(Boolean(e.scriptEnded) && e.rc === 0);
            setLive(false);
            try {
                // A service actor can die while its script stays parked (an
                // uncaught error inside it); the liveness invariant still
                // holds, so nothing else would replace it. Displace the
                // parked bootstrap and park a fresh one.
                if (/not alive/.test(msg)) await stopCurrent(vm);
                await ensureServicesQuiet();
            } catch { /* reported above */ }
        } finally {
            if (!superseded()) {
                busyRef.current = false;
                setRunning(false);
            }
        }
    }

    // Stop the running diagram; the page's services come back in its place.
    async function stopRun() {
        const vm = roxRef.current;
        if (!vm) return;
        ++runSeqRef.current;                      // a pending run is abandoned
        busyRef.current = true;
        try {
            await stopCurrent(vm);
            await ensureServicesQuiet();
            setLive(false);
            setGeneration(g => g + 1);
        } catch (e) {
            setRunError(String(e.message || e));
        } finally {
            busyRef.current = false;
        }
    }

    // The run button's watchdog: bounded steps can still chain into a long
    // wait; do not let the button lie about it.
    useEffect(() => {
        if (!running) return;
        const id = setTimeout(() => {
            busyRef.current = false;
            setRunning(false);
            setRunError('the run is taking unusually long — the VM may be busy '
                      + '(loading a model, or a script that will not stop). Press Run again.');
        }, 45_000);
        return () => clearTimeout(id);
    }, [running]);

    // Liveness: a harness can end on its own (a fatal error); restore the
    // services so the canvas, files and console keep working.
    useEffect(() => {
        if (!rox) return;
        const id = setInterval(async () => {
            if (running || busyRef.current || scriptParked(rox)) return;
            setLive(false);
            try {
                if (await ensureServicesQuiet()) setGeneration(g => g + 1);
            } catch { /* next tick */ }
        }, 1000);
        return () => clearInterval(id);
    }, [rox, running]);

    async function onMenu(action) {
        if (!workspace) return;
        try {
            await ensureServicesQuiet();
            if (action.kind === 'new' || action.kind === 'newDiagram') {
                let name = await askText('New diagram file name', 'pipeline.rox');
                if (!name) return;
                if (!name.endsWith('.rox')) name += '.rox';
                const stem = name.slice(0, -4).replace(/[^A-Za-z0-9]/g, ' ');
                const comp = stem.split(' ').filter(Boolean)
                    .map(w => w[0].toUpperCase() + w.slice(1)).join('') || 'Diagram';
                const r = await dfCall(df, 'create', filePath(name), comp,
                                       [{ name: 'freq', default: 4.0 }]);
                if (!r.ok) { setRunError(r.error); return; }
                await workspace.call('fs_write', filePath(name), r.source);
                await refreshFiles(workspace);
                setSeed(s => ({ ...s, [name]: r.source }));
                await openFile(workspace, name);
            } else if (action.kind === 'open') {
                const text = await openFile(workspace, action.name);
                await runNamed(action.name, { source: text, save: false });
            } else if (action.kind === 'save' && active) {
                await saveFile(workspace, active);
            } else if (action.kind === 'saveAs' && active) {
                let name = await askText('Save as', active);
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
                const list = await refreshFiles(workspace);
                setActive(list[0] ?? null);
            } else if (action.kind === 'reset') {
                localStorage.clear();
                window.location.reload();
            }
        } catch (e) {
            setRunError(String(e.message || e));
        }
    }

    // REPL: a line goes to workspace.eval(); the output delta is the answer.
    // A fatal error takes the parked program down; the watchdog restores the
    // services, and the note says what happened.
    async function evalLine(line) {
        const vm = roxRef.current;
        if (!workspace || !vm) return '(VM not ready)';
        const before = outputRef.current.length;
        const completedBefore = vm.completedCount();
        let err = '';
        const result = await Promise.race([
            workspace.call('eval', line).catch(e => String(e.message || e)),
            new Promise(r => setTimeout(() => r('__eval_timeout__'), 5000)),
        ]);
        if (result !== '__eval_timeout__') err = result || '';
        await new Promise(r => setTimeout(r, 60));
        let note = '';
        if (vm.completedCount() > completedBefore) {
            setLive(false);
            note = '\n(fatal error — the running program ended; press Run to restart it)';
        }
        const delta = outputRef.current.slice(before);
        return (delta + (err ? err + '\n' : '')).replace(/\n$/, '') + note;
    }

    const toggleDrawer = () => setDrawerOpen(v => {
        localStorage.setItem(DRAWER_KEY, v ? 'closed' : 'open');
        return !v;
    });

    // Native menu actions when running inside the Electron shell
    // (web/electron): the page keeps its own File menu too, so nothing here
    // is the only way to do anything.
    const menuRef = useRef(null);
    menuRef.current = async action => {
        switch (action.kind) {
            case 'open-file': {
                if (!workspace) return;
                const list = await refreshFiles(workspace);
                if (!list.includes(action.name)) { setRunError('not in the workspace: ' + action.name); return; }
                const text = await openFile(workspace, action.name);
                await runNamed(action.name, { source: text, save: false });
                break;
            }
            case 'new': case 'save': case 'saveAs':
                await onMenu({ kind: action.kind });
                break;
            case 'run': if (active) runNamed(active); break;
            case 'stop': await stopRun(); break;
            case 'toggle-console': toggleDrawer(); break;
            case 'view': if (activeIsDiagram) setViewMode(action.view); break;
            default: break;
        }
    };
    useEffect(() => {
        const bridge = window.aistudio;
        if (!bridge?.onMenu) return undefined;
        return bridge.onMenu(action => { menuRef.current?.(action); });
    }, []);
    useEffect(() => {
        const onKey = e => {
            if ((e.ctrlKey || e.metaKey) && !e.altKey && e.key.toLowerCase() === 's') {
                e.preventDefault();
                menuRef.current?.({ kind: e.shiftKey ? 'saveAs' : 'save' });
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, []);

    const hostLabel = rox ? (rox.kind === 'socket' ? 'native VM' : 'wasm VM') : '';
    const features = build?.features ?? [];
    const hasNn = features.includes('nn');

    return (
        <div className="ais">
            <header className="ais-top">
                {/* the page's own menu bar sits where a menu bar goes -- the
                    left edge; inside the Electron shell the native menu bar
                    already has these entries, so it is not duplicated */}
                {!window.aistudio && <FileMenu files={files} onAction={onMenu} />}
                <span className="ais-brand">AI Studio</span>
                <span className={'ais-file' + (active && dirty[active] ? ' ais-dirty' : '')}
                      title={active ? filePath(active) + (dirty[active] ? ' — unsaved changes (Ctrl+S)' : '') : ''}>
                    {active ?? 'no file'}{active && dirty[active] ? ' ●' : ''}
                </span>
                {activeIsDiagram &&
                    <span className="view-toggle">
                        {['diagram', 'source'].map(v => (
                            <button key={v} className={'collapse' + (activeView === v ? ' active' : '')}
                                    onClick={() => setViewMode(v)}>{v}</button>
                        ))}
                    </span>}
                <button className="run" disabled={!rox || running || !active}
                        onClick={() => runNamed(active)}>{running ? 'running…' : 'Run'}</button>
                <button className="collapse ais-stop" disabled={!rox || !live}
                        title="stop the running diagram" onClick={stopRun}>Stop</button>
                {runError && <span className={runBenign ? 'run-note' : 'run-error'}>{runError}</span>}
                <span className="ais-status" title={features.join(', ')}>
                    {error ? 'no VM' : rox ? (live ? '● ' : '○ ') + hostLabel : 'connecting…'}
                    {rox && (hasNn ? ' · ai.nn' : ' · no ai.nn')}
                    {build?.version ? ' · ' + build.version : ''}
                </span>
            </header>

            <section className="ais-main">
                {error &&
                    <div className="ais-error">
                        <pre className="error">{error}</pre>
                        <p>Start the native host, then reload:</p>
                        <pre>{'./build/roxal --web-host -p modules --root examples/aistudio'}</pre>
                        <p>or point the page at one with <code>?host=ws://127.0.0.1:8765</code>.</p>
                    </div>}
                {!rox && !error && <p className="loading">connecting to the Roxal VM…</p>}
                {rox && !active && <p className="loading">no diagram in {dataDir} — File → New…</p>}
                {rox && active && (activeView === 'diagram' && df
                    ? <Suspense fallback={<p className="loading">loading the diagram editor…</p>}>
                          <DfEditor key={active}
                                    path={filePath(active)}
                                    getText={() => modelText(active)}
                                    df={df}
                                    rox={rox}
                                    paletteModules={PALETTE_MODULES}
                                    onOpenFile={n => openFile(workspace, n)}
                                    onSource={(src, { initial = false } = {}) => {
                                        writeModel(active, src);
                                        if (initial) return;   // an open, not an edit
                                        setDirty(d => (d[active] ? d : { ...d, [active]: true }));
                                        if (DESKTOP) return;   // explicit Save only
                                        clearTimeout(autosaveRef.current);
                                        const name = active;
                                        autosaveRef.current = setTimeout(
                                            () => { saveFile(workspace, name).catch(() => {}); }, 1000);
                                    }} />
                      </Suspense>
                    : <Editor path={filePath(active)}
                              content={seed[active] ?? ''}
                              onChange={(path, _text) => {
                                  const name = path.slice(dataDirRef.current.length + 1);
                                  setDirty(d => (d[name] ? d : { ...d, [name]: true }));
                              }}
                              service={ide}
                              height="100%" />)}
            </section>

            <section className={'ais-drawer' + (drawerOpen ? ' open' : '')}>
                <div className="ais-drawer-head">
                    <button className="collapse" onClick={toggleDrawer}
                            title={(drawerOpen ? 'hide' : 'show') + ' the console'}>
                        {drawerOpen ? '▾' : '▸'} console
                    </button>
                    {drawerOpen && <>
                        <button className="collapse pane-clear" onClick={() => setOutputClearAt(output.length)}>
                            clear output
                        </button>
                        <button className="collapse pane-clear" onClick={() => setReplGen(g => g + 1)}>
                            clear console
                        </button>
                    </>}
                </div>
                {drawerOpen &&
                    <div className="ais-drawer-body">
                        <pre className="out" ref={outPaneRef}
                             onScroll={e => {
                                 const el = e.currentTarget;
                                 outPinnedRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
                             }}>{output.slice(outputClearAt) || '(nothing yet)'}</pre>
                        <div className="repl-pane">
                            <Repl key={replGen} evalLine={evalLine} />
                        </div>
                    </div>}
            </section>
        </div>
    );
}
