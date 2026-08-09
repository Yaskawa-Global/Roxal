import { useEffect, useRef, useState } from 'react';
import { useRoxal, useRoxalStore } from './roxal-react.js';
import { nnStatus } from './nn-provider.js';

// Draw a digit; Roxal classifies it.
//
// The canvas is the ONLY thing this component owns. Everything shown -- the
// prediction, the confidence, the ten probabilities -- comes from the Roxal
// object the script exposed, and arrives the same way the tanks and oven
// readouts do. Drawing calls one method and then waits, exactly as a robot
// script would hand a camera frame to a model.

const N = 28;              // the model's input is 28x28
const CELL = 11;           // on-screen pixels per model pixel
const SIZE = N * CELL;

// Stroke geometry, in model pixels. MNIST digits were written with a pen and
// then anti-aliased down, so their strokes are a thin solid core with a soft
// edge -- NOT the ~3px slab a fixed 3x3 brush produces, which is far enough
// from the training distribution to cost accuracy.
//
// Ink for a cell is a function of its distance from the stroke's centre line:
// solid within CORE, fading to nothing CORE+FEATHER away. Tune CORE for
// thickness and FEATHER for how soft the edge is.
const CORE = 0.30;         // cells within this distance are fully inked
const FEATHER = 0.70;      // ink falls to zero this much further out

// Distance from a point to a line segment, all in cell units.
function distToSegment(px, py, x0, y0, x1, y1) {
    const dx = x1 - x0, dy = y1 - y0;
    const lenSq = dx * dx + dy * dy;
    let t = lenSq > 0 ? ((px - x0) * dx + (py - y0) * dy) / lenSq : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const cx = x0 + t * dx - px, cy = y0 + t * dy - py;
    return Math.hypot(cx, cy);
}

export default function MnistPanel({ rox }) {
    const digit = useRoxal(rox, 'digit');
    const store = useRoxalStore(rox, 'digit');
    const canvasRef = useRef(null);
    const pixels = useRef(new Float32Array(N * N));
    const drawing = useRef(false);
    const last = useRef(null);        // previous pointer position, in cell units
    const dirty = useRef(false);
    const busy = useRef(false);
    const [hasInk, setHasInk] = useState(false);

    // Repaint one cell rather than the whole grid: drawing stays smooth even
    // on a slow machine, and the canvas is always exactly the model's input.
    function paintCell(ctx, i) {
        const v = pixels.current[i];
        const x = (i % N) * CELL, y = Math.floor(i / N) * CELL;
        ctx.fillStyle = v > 0 ? shade(v) : '#0e1013';
        ctx.fillRect(x, y, CELL, CELL);
    }

    function repaintAll() {
        const ctx = canvasRef.current?.getContext('2d');
        if (!ctx) return;
        for (let i = 0; i < N * N; i++) paintCell(ctx, i);
    }

    useEffect(repaintAll, []);

    // Send the grid to Roxal. One call in flight at a time: the model runs on
    // the VM thread, and queueing every mouse move would build a backlog the
    // user would feel as lag.
    async function classify() {
        if (busy.current || !store) return;
        busy.current = true;
        try {
            while (dirty.current) {
                dirty.current = false;
                await store.call('classify', Array.from(pixels.current));
            }
        } catch (e) {
            // A failed inference must not wedge drawing.
            console.error('[mnist] classify failed', e);
        } finally {
            busy.current = false;
        }
    }

    // Ink the segment from the previous pointer position to this one. Drawing
    // dot-by-dot instead leaves gaps whenever the mouse outruns the event rate
    // -- invisible with a fat brush, obvious with a thin one.
    function ink(e) {
        const canvas = canvasRef.current;
        const rect = canvas.getBoundingClientRect();
        const x1 = (e.clientX - rect.left) / rect.width * N;      // fractional cells
        const y1 = (e.clientY - rect.top) / rect.height * N;
        const prev = last.current;
        const x0 = prev ? prev.x : x1, y0 = prev ? prev.y : y1;
        last.current = { x: x1, y: y1 };

        const reach = CORE + FEATHER;
        const lo = (v, hi) => Math.max(0, Math.min(hi, v));
        const minX = Math.floor(lo(Math.min(x0, x1) - reach, N - 1));
        const maxX = Math.floor(lo(Math.max(x0, x1) + reach, N - 1));
        const minY = Math.floor(lo(Math.min(y0, y1) - reach, N - 1));
        const maxY = Math.floor(lo(Math.max(y0, y1) + reach, N - 1));

        const ctx = canvas.getContext('2d');
        let changed = false;
        for (let cy = minY; cy <= maxY; cy++) {
            for (let cx = minX; cx <= maxX; cx++) {
                // Cell centres, so a stroke down a column of centres is solid.
                const d = distToSegment(cx + 0.5, cy + 0.5, x0, y0, x1, y1);
                if (d >= reach) continue;
                const amount = d <= CORE ? 1 : 1 - (d - CORE) / FEATHER;
                const i = cy * N + cx;
                if (pixels.current[i] >= amount) continue;
                pixels.current[i] = amount;
                paintCell(ctx, i);
                changed = true;
            }
        }
        if (!changed) return;
        setHasInk(true);
        dirty.current = true;
        classify();
    }

    function clear() {
        pixels.current.fill(0);
        repaintAll();
        setHasInk(false);
        dirty.current = false;
        last.current = null;
        store?.call('reset');
    }

    if (digit.device === undefined)
        return <p className="loading">this app exposes no "digit" store — watch the output pane</p>;

    const probs = Array.isArray(digit.probs) ? digit.probs : [];
    const predicted = digit.prediction ?? -1;
    const status = nnStatus();

    return (
        <div className="panel mnist">
            <div className="mnist-main">
                <div>
                    <canvas ref={canvasRef} className="mnist-canvas"
                            width={SIZE} height={SIZE}
                            onPointerDown={e => {
                                e.currentTarget.setPointerCapture(e.pointerId);
                                drawing.current = true;
                                last.current = null;   // a new stroke starts fresh
                                ink(e);
                            }}
                            onPointerMove={e => { if (drawing.current) ink(e); }}
                            onPointerUp={() => { drawing.current = false; last.current = null; }}
                            onPointerLeave={() => { drawing.current = false; last.current = null; }} />
                    <div className="mnist-actions">
                        <button onClick={clear}>clear</button>
                        <span className="mnist-hint">
                            {hasInk ? `${digit.runs ?? 0} inferences` : 'draw a digit here'}
                        </span>
                    </div>
                </div>

                <div className="mnist-verdict">
                    <div className="mnist-digit">{predicted < 0 ? '–' : predicted}</div>
                    <div className="mnist-confidence">
                        {predicted < 0 ? 'waiting' : pct(digit.confidence) + ' sure'}
                    </div>
                    <div className="mnist-device" title={status.reason || ''}>
                        {(digit.device ?? '?') === 'webgpu' ? 'GPU (WebGPU)' : 'CPU'}
                    </div>
                </div>
            </div>

            <div className="mnist-bars">
                {Array.from({ length: 10 }, (_, i) => (
                    <div key={i} className={'mnist-bar' + (i === predicted ? ' win' : '')}>
                        <div className="mnist-bar-track">
                            <div className="mnist-bar-fill"
                                 style={{ height: (100 * (probs[i] ?? 0)).toFixed(1) + '%' }} />
                        </div>
                        <span>{i}</span>
                    </div>
                ))}
            </div>
        </div>
    );
}

const pct = v => Math.round((v ?? 0) * 100) + '%';
// Ink brightness: dim strokes read as the partial values the model receives.
const shade = v => `rgb(${Math.round(120 + 135 * v)}, ${Math.round(200 + 55 * v)}, ${Math.round(140 + 60 * v)})`;
