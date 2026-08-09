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

// A soft round brush, because MNIST was drawn with a pen and the model expects
// strokes with edges rather than single hard cells. Values are ink amounts.
const BRUSH = [
    [0, 0, 1.0], [1, 0, 0.75], [-1, 0, 0.75], [0, 1, 0.75], [0, -1, 0.75],
    [1, 1, 0.4], [1, -1, 0.4], [-1, 1, 0.4], [-1, -1, 0.4],
];

export default function MnistPanel({ rox }) {
    const digit = useRoxal(rox, 'digit');
    const store = useRoxalStore(rox, 'digit');
    const canvasRef = useRef(null);
    const pixels = useRef(new Float32Array(N * N));
    const drawing = useRef(false);
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

    function ink(e) {
        const canvas = canvasRef.current;
        const rect = canvas.getBoundingClientRect();
        const cx = Math.floor((e.clientX - rect.left) / rect.width * N);
        const cy = Math.floor((e.clientY - rect.top) / rect.height * N);
        const ctx = canvas.getContext('2d');
        let changed = false;
        for (const [dx, dy, amount] of BRUSH) {
            const x = cx + dx, y = cy + dy;
            if (x < 0 || x >= N || y < 0 || y >= N) continue;
            const i = y * N + x;
            if (pixels.current[i] >= amount) continue;
            pixels.current[i] = amount;
            paintCell(ctx, i);
            changed = true;
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
                                ink(e);
                            }}
                            onPointerMove={e => { if (drawing.current) ink(e); }}
                            onPointerUp={() => { drawing.current = false; }}
                            onPointerLeave={() => { drawing.current = false; }} />
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
