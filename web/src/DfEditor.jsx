import { useCallback, useEffect, useRef, useState } from 'react';
import {
    ReactFlow, Background, Controls, Handle, Position,
    applyNodeChanges, applyEdgeChanges, BaseEdge, EdgeLabelRenderer,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';
import { dfCall, viewToFlow, opConnect, opDisconnect, opMove } from './dfdoc.js';
import { useRoxal } from './roxal-react.js';
import { askText } from './lib/prompt.js';

// The palette's fixed intrinsics: every diagram can place these regardless of
// which modules are imported. Everything else comes from parsing module
// sources VM-side (dfdoc.palette).
const INTRINSICS = [
    { kind: 'signal', label: 'signal wire', hint: 'declared wire — the state point of any feedback loop' },
    { kind: 'input',  label: 'input',       hint: 'component input port (a :signal init parameter)' },
    { kind: 'output', label: 'output',      hint: 'component output port (a signal property)' },
];
// Base palette modules every diagram offers; the document's own imports add
// their modules on top (see doc_palette), and "+ module…" imports more.
const PALETTE_MODULES = ['math', 'logic'];
// Payload types an input port may declare — mirrors dfdoc's INPUT_TYPES.
const INPUT_TYPES = ['bool', 'byte', 'int', 'real', 'number', 'string',
                     'vector', 'matrix', 'tensor'];

// Payload views an output port may declare -- mirrors dfdoc's OUTPUT_VIEWS.
const OUTPUT_VIEWS = ['text', 'image'];
// Feeds a tensor input may be attached to when the diagram runs (see
// modules/feeds.rox); 'video' asks for a path.
const FEED_CHOICES = [
    ['', 'no feed (stimulus)'], ['camera:0', 'camera 0'], ['camera:1', 'camera 1'],
    ['video', 'video file…'], ['realsense:color', 'RealSense colour'],
    ['realsense:depth', 'RealSense depth'],
];

// Frames per second of a running feed, from the frame count the harness
// exposes as 'feed_<input>' (a SignalView store).
function FeedRate({ rox, name }) {
    const snap = useRoxal(rox, 'feed_' + name);
    const last = useRef({ n: 0, t: 0, fps: null });
    const count = snap?.value;
    useEffect(() => {
        if (typeof count !== 'number') return;
        const now = performance.now();
        const l = last.current;
        if (l.t && now - l.t > 400) {
            l.fps = (count - l.n) * 1000 / (now - l.t);
            l.n = count; l.t = now;
        } else if (!l.t) { l.n = count; l.t = now; }
    }, [count]);
    if (typeof count !== 'number') return null;
    const fps = last.current.fps;
    return <div className="df-fps">{count} frames{fps != null ? ' · ' + fps.toFixed(1) + ' fps' : ''}</div>;
}

// Paints the frames of an image-valued output while its harness runs: the
// harness exposes web.ImageView as 'view_<name>', whose `frame` is the
// output's current tensor (uint8 HxW, HxWx3 or HxWx4; floats are scaled).
function ImagePreview({ rox, name }) {
    const snap = useRoxal(rox, 'view_' + name);
    const ref = useRef(null);
    const frame = snap?.frame;
    useEffect(() => {
        const canvas = ref.current;
        if (!canvas || !frame || !frame.shape) return;
        const [h, w, c = 1] = frame.shape.length === 2 ? [...frame.shape, 1] : frame.shape;
        if (!(h > 0 && w > 0)) return;
        canvas.width = w; canvas.height = h;
        const ctx = canvas.getContext('2d');
        const img = ctx.createImageData(w, h);
        const src = frame.data;
        // uint8 paints as is; floats and wider integers (a uint16 depth map)
        // are scaled by their maximum so the range is visible
        const isFloat = frame.dtype === 'float32' || frame.dtype === 'float64';
        const wide = frame.dtype === 'uint16' || frame.dtype === 'int16' || frame.dtype === 'int32';
        let scale = 1;
        if (isFloat || wide) {
            let max = 0;
            for (let i = 0; i < src.length; i++) if (src[i] > max) max = src[i];
            scale = max > 0 ? (isFloat && max <= 1.5 ? 255 : 255 / max) : 1;
        }
        const px = img.data;
        for (let i = 0, o = 0; i < w * h; i++, o += 4) {
            const b = i * c;
            const v0 = src[b] * scale;
            px[o]     = v0;
            px[o + 1] = c >= 3 ? src[b + 1] * scale : v0;
            px[o + 2] = c >= 3 ? src[b + 2] * scale : v0;
            px[o + 3] = c === 4 ? src[b + 3] * scale : 255;
        }
        ctx.putImageData(img, 0, 0);
    }, [frame]);
    if (!frame || !frame.shape) return <div className="df-preview df-preview-empty">no frame yet</div>;
    return <div className="df-preview"><canvas ref={ref} /></div>;
}

// A store value as one line: numbers trimmed, tensors by shape.
function fmtValue(v) {
    if (v === null || v === undefined) return 'nil';
    if (typeof v === 'number') return Number.isInteger(v) ? String(v) : String(Number(v.toPrecision(6)));
    if (typeof v === 'object') {
        if (v.shape) return 'tensor[' + v.shape.join(', ') + ']' + (v.dtype ? ' ' + v.dtype : '');
        try { return JSON.stringify(v); } catch { return String(v); }
    }
    return String(v);
}

// Shows a text-valued output's current value on the node while its harness
// runs: the harness exposes web.SignalView as 'view_<name>'.
function TextPreview({ rox, name }) {
    const snap = useRoxal(rox, 'view_' + name);
    // nothing until a value arrives: an unprobed output (or a diagram that
    // is not running) has an empty store, and a placeholder on every output
    // read as if something were missing
    if (!snap || typeof snap !== 'object' || !('value' in snap)) return null;
    const text = fmtValue(snap.value);
    return <div className="df-text" title={text}>{text}</div>;
}

// What a node is called on its face and in the properties panel.
function nodeTitle(data) {
    return data.kind === 'func' ? data.callee
         : data.kind === 'diagram' ? data.callee
         : data.kind === 'model' ? data.model
         : Array.isArray(data.name) ? data.name.join(', ') : data.name;
}

// One canvas node. Ports render as React Flow handles: inputs left, outputs
// right, ids 'in<N>' / 'out<N>' to match dfdoc.js's op builders.
function DfNode({ data }) {
    const nIn = (data.kind === 'func' || data.kind === 'diagram' || data.kind === 'model') ? (data.nin ?? 0)
              : (data.kind === 'signal' || data.kind === 'output') ? 1 : 0;
    const nOut = data.kind === 'output' ? 0
               : data.kind === 'diagram' ? (data.outs?.length ?? 1)
               : Array.isArray(data.name) ? data.name.length : 1;
    const title = nodeTitle(data);
    // the wire (or instance) name is what the source and live values key on,
    // so it belongs on the node face -- it is also the only visible effect of
    // renaming a function node
    const wire = Array.isArray(data.name) ? data.name.join(', ') : data.name;
    const sub = { func: '→ ' + wire, diagram: '▣ ' + data.module + '.rox',
                  model: '⚙ ai.nn → ' + wire,
                  signal: data.clock != null ? '@ ' + data.clock : 'signal',
                  input: data.feed ? '⇠ ' + data.feed : null, output: 'output' }[data.kind];
    const imageOut = data.kind === 'output' && data.view === 'image';
    const textOut = data.kind === 'output' && !imageOut;
    return (
        <div className={'df-node df-' + data.kind + (data.diag ? ' df-diag-' + data.diag : '')
                        + (imageOut ? ' df-has-image' : '')}>
            {Array.from({ length: nIn }, (_, i) => (
                <Handle key={'in' + i} id={'in' + i} type="target" position={Position.Left}
                        style={{ top: 14 + i * 14 }} />
            ))}
            <div className="df-title">{title}</div>
            {sub && <div className="df-sub">{sub}</div>}
            {data.kind === 'input' &&
                <select className="df-type nodrag" value={data.type ?? 'real'}
                        onChange={e => data.setType?.(e.target.value)}
                        title="payload type — drives the harness stimulus and checks">
                    {INPUT_TYPES.map(t => <option key={t} value={t}>{t}</option>)}
                </select>}
            {data.kind === 'input' && data.type === 'tensor' &&
                <select className="df-feed nodrag"
                        value={data.feed ? (data.feed.startsWith('video:') ? 'video' : data.feed) : ''}
                        onChange={async e => {
                            const v = e.target.value;
                            if (v === 'video') {
                                const path = await askText('Video file (path on the host)',
                                                           data.feed?.startsWith('video:') ? data.feed.slice(6) : '');
                                if (path && path.trim()) data.setFeed?.('video:' + path.trim());
                            } else data.setFeed?.(v);
                        }}
                        title="what feeds this port when the diagram runs (camera, video file, RealSense)">
                    {FEED_CHOICES.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
                </select>}
            {data.kind === 'input' && data.feed && data.rox &&
                <FeedRate rox={data.rox} name={data.name} />}
            {data.kind === 'output' &&
                <select className="df-view nodrag" value={data.view ?? 'text'}
                        onChange={e => data.setView?.(e.target.value)}
                        title="how the running diagram shows this output: printed, or painted here">
                    {OUTPUT_VIEWS.map(v => <option key={v} value={v}>{v}</option>)}
                </select>}
            {imageOut && data.rox && <ImagePreview rox={data.rox} name={data.name} />}
            {textOut && data.rox && <TextPreview rox={data.rox} name={data.name} />}
            {Array.from({ length: nOut }, (_, i) => (
                <Handle key={'out' + i} id={'out' + i} type="source" position={Position.Right}
                        style={{ top: 14 + i * 14 }} />
            ))}
        </div>
    );
}
const NODE_TYPES = { dfnode: DfNode };

// Palette items grouped by module, in first-seen order: the document's own
// imports come first (they are what this diagram is about), the base
// modules after.
function paletteSections(items) {
    const groups = new Map();
    for (const it of items) {
        if (!groups.has(it.module)) groups.set(it.module, []);
        groups.get(it.module).push(it);
    }
    return Array.from(groups.entries());
}

// A [-1] feedback edge runs right-to-left; routed on the nodes' baseline it
// reads as a stub and its label lands on the forward link. Detour it below
// both nodes, dashed, with the label on the detour.
function FeedbackEdge({ id, sourceX, sourceY, targetX, targetY, label, markerEnd }) {
    const drop = Math.max(sourceY, targetY) + 52;
    const path = `M ${sourceX} ${sourceY} L ${sourceX + 16} ${sourceY} L ${sourceX + 16} ${drop} `
               + `L ${targetX - 16} ${drop} L ${targetX - 16} ${targetY} L ${targetX} ${targetY}`;
    // a third of the way along the detour run: a centered chip sits exactly
    // on the crossing point when links intersect
    const midX = sourceX + (targetX - sourceX) / 3;
    return (<>
        <BaseEdge id={id} path={path} markerEnd={markerEnd} style={{ strokeDasharray: '6 3' }} />
        <EdgeLabelRenderer>
            <div className="df-fb-label"
                 style={{ position: 'absolute', pointerEvents: 'all',
                          transform: `translate(-50%, -50%) translate(${midX}px, ${drop}px)` }}>
                {label ?? '[-1]'}
            </div>
        </EdgeLabelRenderer>
    </>);
}
const EDGE_TYPES = { feedback: FeedbackEdge };

const PROPS_KEY = 'df-props-open';

// The properties panel: what is selected on the canvas (a node, a link) and
// its editable settings, or the diagram's own when nothing is selected.
// Collapsible to a strip on the right edge; the choice persists.
function PropsPanel({ open, setOpen, view, rf, selected, live, wireByNode, palette,
                      apply, renameNode, deleteNodes, editClock, onOpenFile }) {
    const node = selected.nodes.length ? rf.nodes.find(n => n.id === selected.nodes[0].id) : null;
    const edge = !node && selected.edges.length ? rf.edges.find(e => e.id === selected.edges[0].id) : null;
    const byId = id => rf.nodes.find(n => n.id === String(id));
    const row = (k, v) => <div className="df-prop"><span className="k">{k}</span><span className="v" title={String(v)}>{v}</span></div>;

    let body;
    if (node) {
        const d = node.data;
        const hasOuts = d.kind !== 'output';
        // The palette entry this node came from: a function matches by its
        // module-qualified callee, a model by its catalog name (its callee
        // is the call expression, '<instance>.predict').  It carries what
        // the function's docstring says -- dfdoc reads that off the @doc
        // annotation the compiler lifts a """...""" body opener into -- and,
        // for a model, the catalog's own description of it.
        const item = d.kind === 'model'
            ? palette.find(it => it.kind === 'model' && it.name === d.model)
            : (d.callee ? palette.find(it => it.callee === d.callee) : null);
        const doc = item?.doc || '';
        body = (<>
            <h3>{nodeTitle(d)}</h3>
            {doc && <p className="df-doc">{doc}</p>}
            {row('kind', d.kind)}
            {d.kind === 'func' && row('function', d.callee)}
            {d.kind === 'model' && row('model', item?.label || d.model)}
            {d.kind === 'model' && item?.file && row('file', item.file)}
            {d.kind === 'model' && item?.opset != null && row('onnx opset', item.opset)}
            {d.kind === 'diagram' && row('diagram', d.module + '.rox')}
            {d.kind === 'signal' && row('clock', String(d.clock))}
            {d.kind === 'signal' && d.init != null && row('initial', String(d.init))}
            {(d.kind === 'func' || d.kind === 'model' || d.kind === 'signal') &&
                row(Array.isArray(d.name) && d.name.length > 1 ? 'wires' : 'wire',
                    Array.isArray(d.name) ? d.name.join(', ') : d.name)}
            {(d.kind === 'input' || d.kind === 'output' || d.kind === 'diagram') && row('name', d.name)}
            {d.kind === 'input' &&
                <label className="df-prop"><span className="k">type</span>
                    <select value={d.type ?? 'real'} onChange={e => d.setType?.(e.target.value)}>
                        {INPUT_TYPES.map(t => <option key={t} value={t}>{t}</option>)}
                    </select></label>}
            {d.kind === 'input' && d.type === 'tensor' &&
                <label className="df-prop"><span className="k">feed</span>
                    <select value={d.feed ? (d.feed.startsWith('video:') ? 'video' : d.feed) : ''}
                            onChange={async e => {
                                const v = e.target.value;
                                if (v === 'video') {
                                    const path = await askText('Video file (path on the host)',
                                                               d.feed?.startsWith('video:') ? d.feed.slice(6) : '');
                                    if (path && path.trim()) d.setFeed?.('video:' + path.trim());
                                } else d.setFeed?.(v);
                            }}>
                        {FEED_CHOICES.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
                    </select></label>}
            {d.kind === 'input' && d.feed?.startsWith('video:') && row('file', d.feed.slice(6))}
            {d.kind === 'output' &&
                <label className="df-prop"><span className="k">view</span>
                    <select value={d.view ?? 'text'} onChange={e => d.setView?.(e.target.value)}>
                        {OUTPUT_VIEWS.map(v => <option key={v} value={v}>{v}</option>)}
                    </select></label>}
            {row('position', Math.round(node.position.x) + ', ' + Math.round(node.position.y))}
            {hasOuts &&
                <label className="df-prop df-check" title="label this node's outgoing links with their live values while the diagram runs">
                    <input type="checkbox" checked={d.show !== false}
                           onChange={e => apply({ op: 'set_show', id: Number(node.id), show: e.target.checked })} />
                    show values on links
                </label>}
            <div className="df-actions">
                {d.kind === 'diagram'
                    ? <button onClick={() => onOpenFile?.(d.module + '.rox')}>▣ open</button>
                    : <button onClick={() => renameNode(node)}>rename…</button>}
                <button className="df-danger" onClick={() => deleteNodes([node])}>delete</button>
            </div>
        </>);
    } else if (edge) {
        const from = byId(edge.source), to = byId(edge.target);
        const port = Number(String(edge.sourceHandle ?? 'out0').slice(3)) || 0;
        const inPort = Number(String(edge.targetHandle ?? 'in0').slice(2)) || 0;
        const wire = wireByNode[edge.source]?.[port];
        const value = live && wire != null ? live[wire] : undefined;
        const prev = Boolean(edge.data?.prev);
        const retap = tap => apply(opConnect({ source: edge.source, sourceHandle: edge.sourceHandle,
                                               target: edge.target, targetHandle: edge.targetHandle }, tap));
        body = (<>
            <h3>{wire ?? 'link'}</h3>
            {row('kind', 'link')}
            {from && row('from', nodeTitle(from.data) + (port ? ' #' + port : ''))}
            {to && row('to', nodeTitle(to.data) + (inPort ? ' #' + inPort : ''))}
            {row('value', value !== undefined ? String(value) : (live ? '(not sampled)' : '(not running)'))}
            <label className="df-prop df-check" title="read the wire one period ago (a feedback tap)">
                <input type="checkbox" checked={prev} onChange={e => retap(e.target.checked)} /> [-1] tap
            </label>
            {from &&
                <label className="df-prop df-check" title="label this link with its live value while the diagram runs (a setting of the producing node: all its links)">
                    <input type="checkbox" checked={from.data.show !== false}
                           onChange={e => apply({ op: 'set_show', id: Number(from.id), show: e.target.checked })} />
                    show value on link
                </label>}
            <div className="df-actions">
                <button className="df-danger" onClick={() => apply(opDisconnect(edge))}>disconnect</button>
            </div>
        </>);
    } else {
        body = (<>
            <h3>{view?.name ?? '…'}</h3>
            {row('kind', 'diagram')}
            {row('nodes', rf.nodes.length)}
            {row('links', rf.edges.length)}
            {view?.clocks?.map(c => (
                <div className="df-prop" key={c.name}>
                    <span className="k">⏱ {c.name}</span>
                    <button className="df-clock" onClick={() => editClock(c)}
                            title="clock parameter — click to change its default">{String(c.default)} Hz</button>
                </div>
            ))}
            <p className="df-hint">select a node or a link to see its properties</p>
        </>);
    }
    return (
        <aside className={'df-props' + (open ? '' : ' df-collapsed')}>
            <button className="collapse df-props-toggle" onClick={() => setOpen(!open)}
                    title={(open ? 'hide' : 'show') + ' the properties panel'}>
                {open ? '▸ properties' : '◂'}
            </button>
            {open && <div className="df-props-body">{body}</div>}
        </aside>
    );
}

/**
 * The data-flow canvas over one diagram file. The live document is a mirror
 * AST held by the VM's 'df' actor; every interaction here becomes one edit op,
 * and the returned source is pushed into the file's Monaco model via
 * `onSource` so save/dirty/preview keep working unchanged.
 *
 * `getText` reads the CURRENT buffer content for (re)opens — a snapshot prop
 * went stale across view toggles; the Monaco model stays the durable copy,
 * so recovery from any lost VM state is simply another open.
 */
export default function DfEditor({ path, getText, df, onSource, onOpenFile,
                                   paletteModules = PALETTE_MODULES, rox = null }) {
    const [view, setView] = useState(null);
    const [refusal, setRefusal] = useState(null);
    const [palette, setPalette] = useState([]);
    // palette sections folded by the user (module name -> true)
    const [collapsed, setCollapsed] = useState({});
    // bumped whenever an op may have changed the document's imports
    const [paletteGen, setPaletteGen] = useState(0);
    const [components, setComponents] = useState([]);
    const [rf, setRf] = useState({ nodes: [], edges: [] });
    const [diags, setDiags] = useState([]);
    const [selected, setSelected] = useState({ nodes: [], edges: [] });
    const [prevMode, setPrevMode] = useState(false);   // next connection is a [-1] tap
    const placeAt = useRef(40);
    const applyRef = useRef(null);
    // producer node id -> wire name, for labeling edges with live values
    const wireByNode = useRef({});
    // the latest sampled wire values (name -> value), for the properties panel
    const [live, setLive] = useState(null);
    // the properties panel, collapsible; the choice persists per browser
    const [propsOpen, setPropsOpenState] = useState(() => {
        try { return localStorage.getItem(PROPS_KEY) !== 'closed'; } catch { return true; }
    });
    const setPropsOpen = v => {
        setPropsOpenState(v);
        try { localStorage.setItem(PROPS_KEY, v ? 'open' : 'closed'); } catch { /* no storage */ }
    };
    // per-node port payload types + the compatibility table, for the
    // drag-time connection gate (authoritative check re-runs VM-side)
    const portsRef = useRef({});
    const rulesRef = useRef(null);

    // The last source this document produced -- the re-open text whenever the
    // VM-side actor was replaced by a run (each web.serve() starts a fresh
    // registry, so 'no_document' is an expected state, not an error).
    const lastSourceRef = useRef(null);
    const getTextRef = useRef(getText);
    getTextRef.current = getText;
    const bufferText = () => lastSourceRef.current ?? getTextRef.current?.() ?? '';

    // `initial`: the source of a freshly opened document -- its canonical
    // form, which may differ from the file's text without anyone having
    // edited anything, so the app must not mark the file unsaved for it.
    const adopt = useCallback((r, { initial = false } = {}) => {
        if (!r.ok) { setRefusal(r.error); return false; }
        if (r.ports) portsRef.current = r.ports;
        if (r.view) {
            setView(r.view);
            // per-port wire keys: plain wires by name, instance outputs as
            // '<instance>.<out>' (sampled from the sub-diagram's provenance)
            const wires = {};
            for (const n of r.view.nodes ?? []) {
                if (n.kind === 'diagram')
                    wires[String(n.id)] = (n.outs ?? []).map(o => n.name + '.' + o);
                else {
                    const nm = typeof n.name === 'string' ? n.name
                             : (Array.isArray(n.name) && n.name.length === 1 ? n.name[0] : null);
                    wires[String(n.id)] = nm != null ? [nm] : [];
                }
            }
            wireByNode.current = wires;
            const flow = viewToFlow(r.view);
            // input nodes get their payload-type setter injected here, and
            // output nodes their view setter plus the host for previews, so
            // the node component stays a plain renderer
            flow.nodes = flow.nodes.map(n => n.data.kind === 'input'
                ? { ...n, data: { ...n.data, rox,
                    setType: t => applyRef.current?.({ op: 'set_input_type', id: Number(n.id), type: t }),
                    setFeed: f => applyRef.current?.({ op: 'set_input_feed', id: Number(n.id), feed: f }) } }
                : n.data.kind === 'output'
                ? { ...n, data: { ...n.data, rox,
                    setView: v => applyRef.current?.({ op: 'set_output_view', id: Number(n.id), view: v }) } }
                : n);
            // an edit rebuilds the node and edge lists; carry the selection
            // over so a change made from the properties panel keeps its
            // subject selected
            setRf(prev => {
                const selN = new Set(prev.nodes.filter(n => n.selected).map(n => n.id));
                const selE = new Set(prev.edges.filter(e => e.selected).map(e => e.id));
                return {
                    nodes: flow.nodes.map(n => (selN.has(n.id) ? { ...n, selected: true } : n)),
                    edges: flow.edges.map(e => (selE.has(e.id) ? { ...e, selected: true } : e)),
                };
            });
        }
        if (r.source != null) { lastSourceRef.current = r.source; onSource?.(r.source, { initial }); }
        return true;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [onSource, rox]);

    // The compatibility table is static per build; fetch once.
    useEffect(() => {
        let gone = false;
        (async () => {
            const r = await dfCall(df, 'rules');
            if (!gone && r.ok) rulesRef.current = r.rules;
        })();
        return () => { gone = true; };
    }, [df]);

    // Drag-time gate: a connection to a payload-incompatible input refuses
    // to snap. Mirrors dfdoc.compatible -- exact match or a rules-table
    // entry passes, unknown types (null) never block an edit.
    const isValidConnection = useCallback(conn => {
        const rules = rulesRef.current, ports = portsRef.current;
        if (!rules) return true;
        const st = ports[String(conn.source)]?.outs?.[Number(String(conn.sourceHandle ?? 'out0').slice(3))] ?? null;
        const dt = ports[String(conn.target)]?.ins?.[Number(String(conn.targetHandle ?? 'in0').slice(2))] ?? null;
        if (st == null || dt == null || st === dt) return true;
        return Boolean(rules[st]?.includes(dt));
    }, []);

    // One document-scoped store call, transparently re-opening after a run
    // replaced the VM-side registry.
    const docCall = useCallback(async (method, ...args) => {
        let r = await dfCall(df, method, path, ...args);
        if (!r.ok && r.error === 'no_document') {
            const reopened = await dfCall(df, 'open', path, bufferText());
            if (reopened.ok) r = await dfCall(df, method, path, ...args);
        }
        return r;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [df, path]);

    // (Re)open whenever the file changes; the df actor keys documents by path.
    // The buffer may not be readable yet during a boot race, so a failed or
    // empty first open retries once -- an empty canvas that needed a reload
    // was worse than a second attempt.
    useEffect(() => {
        let gone = false;
        (async () => {
            for (let attempt = 0; attempt < 2 && !gone; attempt++) {
                const t = getTextRef.current?.() ?? '';
                if (t) {
                    const r = await dfCall(df, 'open', path, t);
                    if (gone) return;
                    if (r.ok) { setRefusal(null); adopt(r, { initial: true }); return; }
                    setRefusal(r.error);
                }
                await new Promise(res => setTimeout(res, 800));
            }
        })();
        return () => { gone = true; };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [df, path]);

    // The palette and component listings race the boot/run transitions the
    // first time -- keep trying until each answers.
    useEffect(() => {
        let gone = false;
        (async () => {
            for (let i = 0; i < 6 && !gone; i++) {
                // the document's imports first (opened above), then the base
                const r = await docCall('doc_palette', paletteModules);
                if (gone) return;
                if (r.ok) setPalette(r.items.filter(it => it.kind === 'func' || it.kind === 'model'));
                const c = await dfCall(df, 'components', path);
                if (gone) return;
                if (c.ok) setComponents(c.items ?? []);
                if (r.ok && c.ok) return;
                await new Promise(res => setTimeout(res, 2000));
            }
        })();
        return () => { gone = true; };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [df, path, paletteGen]);

    // Diagnostics follow the document: re-check after every adopted view and
    // mark offending nodes.
    useEffect(() => {
        if (!view) return;
        let gone = false;
        (async () => {
            const r = await docCall('check');
            if (gone) return;
            if (r.ok) setDiags(r.diagnostics ?? []);
            else console.warn('df check:', r.error);
        })();
        return () => { gone = true; };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [view]);
    useEffect(() => {
        const worst = {};
        for (const d of diags)
            worst[d.id] = worst[d.id] === 'error' ? 'error' : d.level;
        setRf(s => ({ ...s, nodes: s.nodes.map(n =>
            (n.data.diag ?? null) === (worst[n.id] ?? null) ? n
                : { ...n, data: { ...n.data, diag: worst[n.id] ?? null } }) }));
    }, [diags]);

    // Live values: while a harness instance of this diagram is running, its
    // wires answer to provenance sampling; label the edges with what flows.
    // Cheap polling through borrowed-reference reads -- nothing is injected
    // into the running program.
    useEffect(() => {
        let busy = false;
        const id = setInterval(async () => {
            if (busy) return;
            busy = true;
            const r = await docCall('sample');
            busy = false;
            if (!r.ok && r.error !== 'no_document') console.warn('df sample:', r.error);
            const values = r.ok ? r.values : null;
            setLive(values);
            // a producer whose 'show' is off keeps its links unlabeled
            setRf(s => {
                const shown = {};
                for (const n of s.nodes) shown[n.id] = n.data.show !== false;
                return { ...s, edges: s.edges.map(e => {
                    const port = Number(String(e.sourceHandle ?? 'out0').slice(3)) || 0;
                    const wire = wireByNode.current[e.source]?.[port];
                    const v = values && wire != null && shown[e.source] ? values[wire] : undefined;
                    const tap = e.data?.prev ? '[-1]' : '';
                    const label = v !== undefined ? (tap ? tap + ' ' : '') + v : (tap || undefined);
                    return label === e.label ? e : { ...e, label };
                }) };
            });
        }, 400);
        return () => clearInterval(id);
    }, [docCall]);

    // One edit op; on 'no_document' (VM restarted underneath us) re-open from
    // the buffer and retry once.
    const apply = useCallback(async op => {
        const r = await docCall('apply', op);
        if (!r.ok) setRefusal(r.error);
        else {
            setRefusal(null);
            adopt(r);
            if (op.op === 'add_node' || op.op === 'add_import') setPaletteGen(g => g + 1);
        }
        return r;
    }, [docCall, adopt]);

    // Import another module so its functions join the palette.
    const addModule = async () => {
        const mod = await askText('Module to import (its typed functions become nodes)', 'opencv');
        if (!mod || !mod.trim()) return;
        await apply({ op: 'add_import', module: mod.trim() });
    };
    applyRef.current = apply;

    const addNode = async spec => {
        const y = (placeAt.current = (placeAt.current + 60) % 420);
        if (spec.kind === 'component') {
            await apply({ op: 'add_node', kind: 'diagram',
                          module: spec.module, type: spec.type,
                          nin: spec.inputs.length, outs: spec.outs, clocks: spec.clocks,
                          x: 260, y });
        } else if (spec.kind === 'func') {
            // ports for the required parameters only; defaulted ones keep
            // their defaults (a threshold, a size) until a literal editor exists
            const required = spec.params.filter(p => !p.has_default).length;
            await apply({ op: 'add_node', kind: 'func', callee: spec.callee,
                          nin: required, nout: Math.max(1, spec.outputs.length),
                          x: 260, y });
        } else if (spec.kind === 'model') {
            await apply({ op: 'add_node', kind: 'model', name: spec.name, file: spec.file,
                          nin: spec.params.length, nout: Math.max(1, spec.outputs.length),
                          x: 260, y });
        } else if (spec.kind === 'signal') {
            const clock = view?.clocks?.[0]?.name ?? 4.0;
            await apply({ op: 'add_node', kind: 'signal', label: 'w', clock, init: 0.0, x: 120, y });
        } else {
            const name = await askText(spec.kind + ' name', spec.kind === 'input' ? 'in1' : 'out1');
            if (!name) return;
            await apply({ op: 'add_node', kind: spec.kind, name,
                          x: spec.kind === 'input' ? 20 : 460, y });
        }
    };

    const deleteEdges = useCallback(async edges => {
        for (const e of edges) await applyRef.current?.(opDisconnect(e));
    }, []);
    const deleteNodes = useCallback(async nodes => {
        for (const n of nodes) await applyRef.current?.({ op: 'remove_node', id: Number(n.id) });
    }, []);
    const deleteSelected = useCallback(async () => {
        await deleteEdges(selected.edges);
        await deleteNodes(selected.nodes);
        setSelected({ nodes: [], edges: [] });
    }, [selected, deleteEdges, deleteNodes]);

    // Drags stay local until drag-stop, then persist as one move op.
    const onNodesChange = useCallback(changes => {
        setRf(s => ({ ...s, nodes: applyNodeChanges(changes, s.nodes) }));
    }, []);
    // Without this, edge clicks only FOCUS (grey) -- the select change is
    // never applied to controlled state, so Delete and the button stay dead.
    const onEdgesChange = useCallback(changes => {
        setRf(s => ({ ...s, edges: applyEdgeChanges(changes, s.edges) }));
    }, []);
    const onNodeDragStop = useCallback((_e, node) => { apply(opMove(node)); }, [apply]);
    const onConnect = useCallback(conn => { apply(opConnect(conn, prevMode)); }, [apply, prevMode]);
    const onSelectionChange = useCallback(sel => {
        setSelected({ nodes: sel.nodes ?? [], edges: sel.edges ?? [] });
    }, []);
    // Double-click an edge to toggle its [-1] delay tap (single click selects,
    // so Delete and the 🗑 button work the way other editors do).
    const onEdgeDoubleClick = useCallback((_e, edge) => {
        apply(opConnect({ source: edge.source, sourceHandle: edge.sourceHandle,
                          target: edge.target, targetHandle: edge.targetHandle },
                        !edge.data?.prev));
    }, [apply]);
    // Drag an edge endpoint to another port: one disconnect + one connect,
    // keeping the delay tap.
    const onReconnect = useCallback((oldEdge, conn) => {
        (async () => {
            await applyRef.current?.(opDisconnect(oldEdge));
            await applyRef.current?.(opConnect(conn, Boolean(oldEdge.data?.prev)));
        })();
    }, []);
    const renameNode = useCallback(async node => {
        const current = typeof node.data.name === 'string' ? node.data.name : '';
        const name = await askText('rename ' + (node.data.callee ?? current), current);
        if (name && name !== current)
            applyRef.current?.({ op: 'rename', id: Number(node.id), name });
    }, []);
    // Double-click: a diagram node drills into its file; anything else renames
    // (multi-output nodes refuse VM-side).
    const onNodeDoubleClick = useCallback((_e, node) => {
        if (node.data.kind === 'diagram') onOpenFile?.(node.data.module + '.rox');
        else renameNode(node);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [onOpenFile, renameNode]);

    // Right-click menus -- the conventional home for drill-in and destructive
    // actions. Position is viewport-fixed; any pane interaction dismisses.
    const [menu, setMenu] = useState(null);
    const onNodeContextMenu = useCallback((e, node) => {
        e.preventDefault();
        const items = [];
        if (node.data.kind === 'diagram')
            items.push({ label: '▣ open ' + node.data.module + '.rox',
                         act: () => onOpenFile?.(node.data.module + '.rox') });
        else
            items.push({ label: 'rename…', act: () => renameNode(node) });
        items.push({ label: 'delete', act: () => deleteNodes([node]) });
        setMenu({ x: e.clientX, y: e.clientY, items });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [onOpenFile, renameNode]);
    const onEdgeContextMenu = useCallback((e, edge) => {
        e.preventDefault();
        setMenu({ x: e.clientX, y: e.clientY, items: [
            { label: (edge.data?.prev ? 'remove' : 'add') + ' [-1] tap',
              act: () => apply(opConnect({ source: edge.source, sourceHandle: edge.sourceHandle,
                                           target: edge.target, targetHandle: edge.targetHandle },
                                         !edge.data?.prev)) },
            { label: 'disconnect', act: () => apply(opDisconnect(edge)) },
        ]});
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [apply]);
    const editClock = useCallback(async c => {
        const v = await askText('default rate for ' + c.name + ' (Hz)', String(c.default ?? 1));
        if (v == null || v === '') return;
        const hz = Number(v);
        if (!Number.isFinite(hz) || hz <= 0) return;
        applyRef.current?.({ op: 'set_clock', name: c.name, default: hz });
    }, []);

    if (refusal && !view) {
        return (
            <div className="df-refusal">
                <p><b>not diagram-editable:</b> {refusal}</p>
                <p>switch to the source view to edit this file as text.</p>
            </div>
        );
    }

    const nSelected = selected.nodes.length + selected.edges.length;
    return (
        <div className="df-editor">
            <aside className="df-palette">
                <h3>{view?.name ?? '…'}</h3>
                {view?.clocks?.map(c => (
                    <button key={c.name} className="df-clock"
                            title="clock parameter — click to change its default; consumers may override"
                            onClick={() => editClock(c)}>
                        ⏱ {c.name} = {String(c.default)} Hz
                    </button>
                ))}
                <label className="df-prev" title="the next connection reads the wire one period ago">
                    <input type="checkbox" checked={prevMode}
                           onChange={e => setPrevMode(e.target.checked)} /> [-1] tap
                </label>
                <button className="df-delete" disabled={!nSelected} onClick={deleteSelected}
                        title="remove the selected nodes and connections (also: select + Delete key)">
                    🗑 delete selected{nSelected ? ' (' + nSelected + ')' : ''}
                </button>
                <div className="df-sep" />
                {INTRINSICS.map(it => (
                    <button key={it.kind} title={it.hint} onClick={() => addNode(it)}>{it.label}</button>
                ))}
                {paletteSections(palette).map(([mod, items]) => (
                    <div key={mod} className={'df-section' + (collapsed[mod] ? ' df-collapsed' : '')}>
                        <div className="df-sep" />
                        <h4 onClick={() => setCollapsed(c => ({ ...c, [mod]: !c[mod] }))}
                            title={(collapsed[mod] ? 'show' : 'hide') + ' ' + mod}>
                            {collapsed[mod] ? '▸' : '▾'} {mod} <span className="df-count">{items.length}</span>
                        </h4>
                        {!collapsed[mod] && items.map(it => (
                            <button key={it.callee}
                                    title={(it.doc ? it.doc + '\n\n' : '')
                                           + it.params.map(p => p.name + (p.type ? ' :' + p.type : '')).join(', ')
                                           + ' → ' + (it.outputs.join(', ') || '?')}
                                    onClick={() => addNode(it)}>
                                {it.name}
                            </button>
                        ))}
                    </div>
                ))}
                <div className="df-sep" />
                <button className="df-add-module" onClick={addModule}
                        title="import a module; its typed functions join the palette">
                    + module…
                </button>
                {components.length > 0 && <div className="df-sep" />}
                {components.map(it => (
                    <button key={it.file} className="df-component"
                            title={it.file + ': (' + it.inputs.map(p => p.name + ' :' + p.type).join(', ')
                                   + ') → ' + it.outs.join(', ')}
                            onClick={() => addNode({ ...it, kind: 'component' })}>
                        ▣ {it.type}
                    </button>
                ))}
                {diags.length > 0 && <div className="df-sep" />}
                {diags.map((d, i) => (
                    <div key={i} className={'df-diag df-diag-' + d.level}>{d.message}</div>
                ))}
                {refusal && <p className="df-error">{refusal}</p>}
            </aside>
            <div className="df-canvas">
                <ReactFlow
                    nodes={rf.nodes} edges={rf.edges} nodeTypes={NODE_TYPES} edgeTypes={EDGE_TYPES}
                    onNodesChange={onNodesChange} onEdgesChange={onEdgesChange}
                    onNodeDragStop={onNodeDragStop}
                    onConnect={onConnect} onEdgeDoubleClick={onEdgeDoubleClick}
                    onReconnect={onReconnect} onNodeDoubleClick={onNodeDoubleClick}
                    onEdgesDelete={deleteEdges} onNodesDelete={deleteNodes}
                    onSelectionChange={onSelectionChange}
                    onNodeContextMenu={onNodeContextMenu} onEdgeContextMenu={onEdgeContextMenu}
                    isValidConnection={isValidConnection}
                    onPaneClick={() => setMenu(null)} onMoveStart={() => setMenu(null)}
                    deleteKeyCode={['Backspace', 'Delete']}
                    snapToGrid snapGrid={[16, 16]}
                    fitView proOptions={{ hideAttribution: true }}>
                    <Background gap={16} />
                    <Controls showInteractive={false} />
                </ReactFlow>
                {menu &&
                    <div className="df-menu" style={{ left: menu.x, top: menu.y }}
                         onMouseLeave={() => setMenu(null)}>
                        {menu.items.map((m, i) => (
                            <button key={i} onClick={() => { setMenu(null); m.act(); }}>{m.label}</button>
                        ))}
                    </div>}
            </div>
            <PropsPanel open={propsOpen} setOpen={setPropsOpen} view={view} rf={rf}
                        selected={selected} live={live} wireByNode={wireByNode.current}
                        palette={palette}
                        apply={apply} renameNode={renameNode} deleteNodes={deleteNodes}
                        editClock={editClock} onOpenFile={onOpenFile} />
        </div>
    );
}
