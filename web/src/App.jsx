import { useEffect, useMemo, useState } from 'react';
import { startRoxal, runScript } from './roxal.js';
import Editor from './Editor.jsx';
import { useRoxal, useRoxalStore } from './roxal-react.js';
import APP_SRC from './app.rox.js';

// Everything below is ordinary React. The only Roxal-aware lines are the two
// hooks -- useRoxal for state, useRoxalStore for actions. No effects to wire, no
// polling, no message plumbing: the store is an external store and React already
// knows how to consume one.
function OvenPanel({ rox }) {
    const oven = useRoxal(rox, 'oven');
    const store = useRoxalStore(rox, 'oven');

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

export default function App() {
    const [rox, setRox] = useState(null);
    const [error, setError] = useState(null);
    const [output, setOutput] = useState('');
    const [source, setSource] = useState(APP_SRC);
    const [dirty, setDirty] = useState(false);
    const [running, setRunning] = useState(false);
    const [runError, setRunError] = useState(null);
    // Memoised, not `rox && rox.roxalStore('ide')` inline: roxalStore returns a
    // fresh handle each call, so an inline one would change identity on every
    // render and re-trigger the editor's diagnostics effect at the network's
    // update rate. Stable per VM; null until one exists.
    const ide = useMemo(() => (rox ? rox.roxalStore('ide') : null), [rox]);
    // Remount the panel after a re-run so hooks re-read the replaced store.
    const [generation, setGeneration] = useState(0);

    async function rerun() {
        setRunning(true);
        setRunError(null);
        try {
            await runScript(rox, source, { expectStore: 'oven' });
            setDirty(false);
            setGeneration(g => g + 1);
        } catch (e) {
            setRunError(String(e.message || e));
        } finally {
            setRunning(false);
        }
    }

    useEffect(() => {
        startRoxal(APP_SRC, { expectStore: 'oven', onOutput: setOutput })
            .then(({ rox }) => setRox(rox))
            .catch(e => setError(String(e.message || e)));
    }, []);

    return (
        <main>
            <header>
                <h1>Roxal + React</h1>
                <p className="sub">
                    The oven's plant and control law are a Roxal <b>signal network</b> — no update
                    loop, just a dataflow graph the engine evaluates at 20&nbsp;Hz. React renders it
                    through <code>useSyncExternalStore</code> and knows nothing else.
                </p>
            </header>

            <div className="workbench">
                {/* left: the source */}
                <section className="pane editor-pane">
                    <div className="pane-head">
                        <h2>app.rox</h2>
                        <button className="run" disabled={!rox || running}
                                onClick={rerun}>{running ? 'running…' : 'Run'}</button>
                        {dirty && !running && <span className="dirty">edited — press Run</span>}
                        {runError && <span className="run-error">{runError}</span>}
                    </div>
                    <Editor value={APP_SRC}
                            onChange={src => { setSource(src); setDirty(true); }}
                            service={ide}
                            height="100%" />
                </section>

                {/* right: the running app above, its output below */}
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
                </div>
            </div>
        </main>
    );
}
