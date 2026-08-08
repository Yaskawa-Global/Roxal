import { useRoxal, useRoxalStore } from './roxal-react.js';

// The view for tanks.rox. Every number below is a Roxal signal arriving at
// 20 Hz; nothing here simulates anything. The animation is CSS -- water that
// moves on its own -- while the WATER LEVEL is pure data from the network.

const fmt = (v, p = 2) => (typeof v === 'number' ? v.toFixed(p) : '–');

// Two offset wave layers make a surface look like it is moving even when the
// level is still; the level itself only ever comes from the signal.
function Waves({ tone }) {
    return (
        <div className="waves">
            {[0, 1].map(i => (
                <svg key={i} className={'wave wave' + i} viewBox="0 0 240 20" preserveAspectRatio="none">
                    <path d="M0,10 C20,2 40,18 60,10 C80,2 100,18 120,10 C140,2 160,18 180,10 C200,2 220,18 240,10 L240,20 L0,20 Z"
                          fill={tone} />
                </svg>
            ))}
        </div>
    );
}

function Tank({ label, level, capacity, spilling, inflowing, feed, feedLabel, drop }) {
    const pct = Math.max(0, Math.min(100, (level / capacity) * 100));
    return (
        <div className={'tank-unit' + (drop ? ' drop' : '')}>
            {/* the inlet pours in from directly above, and each tank sits lower
                than the one feeding it, so the eye follows the water down */}
            <span className="k feed-label">{feedLabel}</span>
            <div className="inlet"><Stream rate={feed} /></div>
            <div className={'tank' + (spilling ? ' spilling' : '')}>
                {[25, 50, 75].map(t => (
                    <div key={t} className="tick" style={{ bottom: t + '%' }} />
                ))}
                <div className="water" style={{ height: pct + '%' }}>
                    {/* the crest sits ABOVE the water box, so it must not be
                        clipped -- only the bubbles are, which is why they get
                        their own clipping layer rather than overflow on .water */}
                    <Waves tone="rgba(125, 211, 252, .55)" />
                    <div className="bubbles">
                        {inflowing && pct > 4 && [0, 1, 2, 3, 4].map(i => (
                            <span key={i} className="bubble"
                                  style={{ left: 12 + i * 15 + '%', animationDelay: i * 0.45 + 's' }} />
                        ))}
                    </div>
                </div>
                {spilling && <div className="spill-lip" />}
                <div className="gloss" />
            </div>
            <div className="tank-label">
                <span className="k">{label}</span>
                <span className="v big">{fmt(level)}<em>m</em></span>
            </div>
        </div>
    );
}

// A falling stream whose thickness and speed follow the flow rate.
function Stream({ rate, tall }) {
    const on = rate > 0.02;
    const w = Math.max(3, Math.min(14, rate * 2.6));
    return (
        <div className={'stream' + (tall ? ' tall' : '') + (on ? '' : ' dry')}
             style={{ width: w + 'px', animationDuration: Math.max(0.25, 1.2 - rate * 0.15) + 's' }}>
            <span className="splash" style={{ opacity: on ? Math.min(1, rate / 3) : 0 }} />
        </div>
    );
}

export default function TanksPanel({ rox }) {
    const t = useRoxal(rox, 'tanks');
    const store = useRoxalStore(rox, 'tanks');

    if (t.level1 === undefined)
        return <p className="loading">this app exposes no "tanks" store — watch the output pane</p>;

    const capacity = t.capacity ?? 10;
    const inflow = t.inflow ?? 0;

    return (
        <div className="panel tanks-panel">
            {/* A cascade runs downhill, so the tanks stack: the tap pours into
                tank 1, tank 1 spills into tank 2, tank 2 runs to the drain. */}
            <div className="rig">
                <Tank label="tank 1" level={t.level1 ?? 0} capacity={capacity}
                      spilling={Boolean(t.spilling)} inflowing={inflow > 0.05}
                      feed={inflow} feedLabel="tap" />
                <Tank label="tank 2" level={t.level2 ?? 0} capacity={capacity}
                      spilling={false} inflowing={(t.out1 ?? 0) > 0.05}
                      feed={t.out1 ?? 0} feedLabel="1 → 2" drop />
                <div className="outlet">
                    <Stream rate={t.out2 ?? 0} />
                    <span className="k">drain</span>
                </div>
            </div>

            <div className="tanks-controls">
            <div className="readout tanks-readout">
                <Field label="inflow" value={fmt(inflow)} unit="m³/s" />
                <Field label="1 → 2" value={fmt(t.out1 ?? 0)} unit="m³/s" />
                <Field label="outflow" value={fmt(t.out2 ?? 0)} unit="m³/s" />
                <Field label="stored" value={fmt(t.stored ?? 0)} unit="m"
                       tone={t.spilling ? 'busy' : t.settled ? 'calm' : ''} />
            </div>

            <label className="slider">
                <span>tap</span>
                {/* Writes straight into the Roxal signal; the network takes it from there. */}
                <input type="range" min="0" max="7" step="0.1" value={inflow}
                       onChange={e => store.set('inflow', Number(e.target.value))} />
                <b>{fmt(inflow, 1)}</b>
            </label>

            <div className="actions">
                <button onClick={() => store.call('shut')}>shut</button>
                <button onClick={() => store.call('trickle')}>trickle</button>
                <button onClick={() => store.call('steady')}>steady</button>
                <button onClick={() => store.call('surge')}>surge</button>
            </div>

            <p className={'log' + (t.spilling ? ' alarm' : '')}>{t.phase}</p>
            </div>
        </div>
    );
}

function Field({ label, value, unit, tone }) {
    return (
        <div className="field">
            <span className="k">{label}</span>
            <span className={'v' + (tone ? ' ' + tone : '')}>
                {value ?? '–'}{unit && <em className="unit">{unit}</em>}
            </span>
        </div>
    );
}
