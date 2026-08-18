// Client half of the data-flow diagram editor: sniffing diagram files,
// contained calls to the 'df' store (a Roxal actor holding the live mirror-AST
// documents), and the view-JSON <-> React Flow shape mapping. All semantic
// work happens VM-side in modules/dfdoc.rox -- this file renders and relays.

// A diagram file declares itself with a leading file-level marker annotation.
// Comments and blank lines may precede it; anything else means "not a diagram".
export function isDiagramSource(text) {
    for (const line of String(text ?? '').split('\n')) {
        const t = line.trim();
        if (t === '' || t.startsWith('//') || t.startsWith('#')) continue;
        return t.startsWith('@dataflow_diagram(');
    }
    return false;
}

// One contained call: bounded by a timeout (a wedged VM otherwise hangs the
// canvas forever) and normalized to the {ok: ...} envelope the Df actor
// speaks. Never throws.
export async function dfCall(store, method, ...args) {
    if (!store) return { ok: false, error: 'VM not ready' };
    try {
        const r = await Promise.race([
            store.call(method, ...args),
            new Promise(res => setTimeout(() => res({ ok: false, error: 'timeout' }), 10_000)),
        ]);
        return (r && typeof r === 'object') ? r : { ok: false, error: String(r) };
    } catch (e) {
        return { ok: false, error: String(e.message || e) };
    }
}

// view JSON -> React Flow nodes/edges. Node ids are stringified document ids;
// port handles are 'in<N>' / 'out<N>'. Edge ids encode their target port,
// which is unique -- a port has at most one producer.
export function viewToFlow(view) {
    const nodes = (view?.nodes ?? []).map(n => ({
        id: String(n.id),
        type: 'dfnode',
        position: { x: n.x ?? 0, y: n.y ?? 0 },
        data: n,
    }));
    const edges = (view?.edges ?? []).filter(e => e.from).map(e => ({
        id: 'e' + e.to.id + ':' + e.to.port,
        source: String(e.from.id),
        sourceHandle: 'out' + e.from.port,
        target: String(e.to.id),
        targetHandle: 'in' + e.to.port,
        label: e.prev ? '[-1]' : undefined,
        animated: Boolean(e.splice),
        // forward edges route orthogonally; [-1] feedback edges get their own
        // dashed detour below the nodes (FeedbackEdge in DfEditor.jsx)
        type: e.prev ? 'feedback' : 'smoothstep',
        markerEnd: { type: 'arrowclosed', width: 14, height: 14 },
        reconnectable: true,
        data: { prev: Boolean(e.prev) },
    }));
    return { nodes, edges };
}

// React Flow connection/selection objects -> dfdoc edit ops.
export const opConnect = (conn, prev = false) => ({
    op: 'connect',
    from: { id: Number(conn.source), port: Number(String(conn.sourceHandle ?? 'out0').slice(3)) },
    to: { id: Number(conn.target), port: Number(String(conn.targetHandle ?? 'in0').slice(2)) },
    prev,
});
export const opDisconnect = edge => ({
    op: 'disconnect',
    to: { id: Number(edge.target), port: Number(String(edge.targetHandle ?? 'in0').slice(2)) },
});
export const opMove = node => ({
    op: 'move',
    id: Number(node.id),
    x: Math.round(node.position.x),
    y: Math.round(node.position.y),
});
