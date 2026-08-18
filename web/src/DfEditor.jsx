import { useCallback, useEffect, useRef, useState } from 'react';
import {
    ReactFlow, Background, Controls, Handle, Position,
    applyNodeChanges, applyEdgeChanges, BaseEdge, EdgeLabelRenderer,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';
import { dfCall, viewToFlow, opConnect, opDisconnect, opMove } from './dfdoc.js';

// The palette's fixed intrinsics: every diagram can place these regardless of
// which modules are imported. Everything else comes from parsing module
// sources VM-side (dfdoc.palette).
const INTRINSICS = [
    { kind: 'signal', label: 'signal wire', hint: 'declared wire — the state point of any feedback loop' },
    { kind: 'input',  label: 'input',       hint: 'component input port (a :signal init parameter)' },
    { kind: 'output', label: 'output',      hint: 'component output port (a signal property)' },
];
// M1: the palette modules are fixed; diagram-options grow this later.
const PALETTE_MODULES = ['math', 'logic'];
// Payload types an input port may declare — mirrors dfdoc's INPUT_TYPES.
const INPUT_TYPES = ['bool', 'byte', 'int', 'real', 'number', 'string',
                     'vector', 'matrix', 'tensor'];

// One canvas node. Ports render as React Flow handles: inputs left, outputs
// right, ids 'in<N>' / 'out<N>' to match dfdoc.js's op builders.
function DfNode({ data }) {
    const nIn = (data.kind === 'func' || data.kind === 'diagram') ? (data.nin ?? 0)
              : (data.kind === 'signal' || data.kind === 'output') ? 1 : 0;
    const nOut = data.kind === 'output' ? 0
               : data.kind === 'diagram' ? (data.outs?.length ?? 1)
               : Array.isArray(data.name) ? data.name.length : 1;
    const title = data.kind === 'func' ? data.callee
                : data.kind === 'diagram' ? data.callee
                : Array.isArray(data.name) ? data.name.join(', ') : data.name;
    // the wire (or instance) name is what the source and live values key on,
    // so it belongs on the node face -- it is also the only visible effect of
    // renaming a function node
    const wire = Array.isArray(data.name) ? data.name.join(', ') : data.name;
    const sub = { func: '→ ' + wire, diagram: '▣ ' + data.module + '.rox',
                  signal: data.clock != null ? '@ ' + data.clock : 'signal',
                  input: null, output: 'output' }[data.kind];
    return (
        <div className={'df-node df-' + data.kind + (data.diag ? ' df-diag-' + data.diag : '')}>
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
            {Array.from({ length: nOut }, (_, i) => (
                <Handle key={'out' + i} id={'out' + i} type="source" position={Position.Right}
                        style={{ top: 14 + i * 14 }} />
            ))}
        </div>
    );
}
const NODE_TYPES = { dfnode: DfNode };

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
export default function DfEditor({ path, getText, df, onSource, onOpenFile }) {
    const [view, setView] = useState(null);
    const [refusal, setRefusal] = useState(null);
    const [palette, setPalette] = useState([]);
    const [components, setComponents] = useState([]);
    const [rf, setRf] = useState({ nodes: [], edges: [] });
    const [diags, setDiags] = useState([]);
    const [selected, setSelected] = useState({ nodes: [], edges: [] });
    const [prevMode, setPrevMode] = useState(false);   // next connection is a [-1] tap
    const placeAt = useRef(40);
    const applyRef = useRef(null);
    // producer node id -> wire name, for labeling edges with live values
    const wireByNode = useRef({});
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

    const adopt = useCallback(r => {
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
            // input nodes get their payload-type setter injected here so the
            // node component stays a plain renderer
            flow.nodes = flow.nodes.map(n => n.data.kind === 'input'
                ? { ...n, data: { ...n.data,
                    setType: t => applyRef.current?.({ op: 'set_input_type', id: Number(n.id), type: t }) } }
                : n);
            setRf(flow);
        }
        if (r.source != null) { lastSourceRef.current = r.source; onSource?.(r.source); }
        return true;
    }, [onSource]);

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
                    if (r.ok) { setRefusal(null); adopt(r); return; }
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
                const r = await dfCall(df, 'palette', PALETTE_MODULES);
                if (gone) return;
                if (r.ok) setPalette(r.items.filter(it => it.kind === 'func'));
                const c = await dfCall(df, 'components', path);
                if (gone) return;
                if (c.ok) setComponents(c.items ?? []);
                if (r.ok && c.ok) return;
                await new Promise(res => setTimeout(res, 2000));
            }
        })();
        return () => { gone = true; };
    }, [df, path]);

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
            setRf(s => ({ ...s, edges: s.edges.map(e => {
                const port = Number(String(e.sourceHandle ?? 'out0').slice(3)) || 0;
                const wire = wireByNode.current[e.source]?.[port];
                const v = values && wire != null ? values[wire] : undefined;
                const tap = e.data?.prev ? '[-1]' : '';
                const label = v !== undefined ? (tap ? tap + ' ' : '') + v : (tap || undefined);
                return label === e.label ? e : { ...e, label };
            }) }));
        }, 400);
        return () => clearInterval(id);
    }, [docCall]);

    // One edit op; on 'no_document' (VM restarted underneath us) re-open from
    // the buffer and retry once.
    const apply = useCallback(async op => {
        const r = await docCall('apply', op);
        if (!r.ok) setRefusal(r.error);
        else { setRefusal(null); adopt(r); }
        return r;
    }, [docCall, adopt]);
    applyRef.current = apply;

    const addNode = async spec => {
        const y = (placeAt.current = (placeAt.current + 60) % 420);
        if (spec.kind === 'component') {
            await apply({ op: 'add_node', kind: 'diagram',
                          module: spec.module, type: spec.type,
                          nin: spec.inputs.length, outs: spec.outs, clocks: spec.clocks,
                          x: 260, y });
        } else if (spec.kind === 'func') {
            await apply({ op: 'add_node', kind: 'func', callee: spec.callee,
                          nin: spec.params.length, nout: Math.max(1, spec.outputs.length),
                          x: 260, y });
        } else if (spec.kind === 'signal') {
            const clock = view?.clocks?.[0]?.name ?? 4.0;
            await apply({ op: 'add_node', kind: 'signal', label: 'w', clock, init: 0.0, x: 120, y });
        } else {
            const name = window.prompt(spec.kind + ' name', spec.kind === 'input' ? 'in1' : 'out1');
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
    const renameNode = useCallback(node => {
        const current = typeof node.data.name === 'string' ? node.data.name : '';
        const name = window.prompt('rename ' + (node.data.callee ?? current), current);
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
    const editClock = useCallback(c => {
        const v = window.prompt('default rate for ' + c.name + ' (Hz)', String(c.default ?? 1));
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
                <div className="df-sep" />
                {palette.map(it => (
                    <button key={it.callee}
                            title={it.params.map(p => p.name + (p.type ? ' :' + p.type : '')).join(', ')
                                   + ' → ' + (it.outputs.join(', ') || '?')}
                            onClick={() => addNode(it)}>
                        {it.name}
                    </button>
                ))}
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
        </div>
    );
}
