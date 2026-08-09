// The host NN provider for Roxal's ai.nn module, backed by ONNX Runtime.
//
// One factory serves every host: the browser passes onnxruntime-web (WebGPU
// EP with wasm fallback), the node test runner passes the same package (wasm
// EP), and an Electron host can pass onnxruntime-node for native GPU. The
// object it returns is what wasm/roxal-bridge.js dispatches Op::NnRequest to
// as Module.roxalNN:
//
//   create(modelBytes: Uint8Array, device) -> Promise<{id, device, inputs, outputs}>
//   run(id, feeds: [{name, dtype, shape, data: Uint8Array}]) -> Promise<[...outputs]>
//   close(id)
//
// device strings: 'auto' prefers webgpu when opts.webgpu says an adapter
// exists, else the wasm EP; 'webgpu' demands it; 'cpu' forces the wasm EP.
// The reply's device field reports where the session actually landed --
// 'webgpu' or 'cpu' (the wasm EP IS CPU execution), matching the native
// vocabulary so scripts asserting on model.device() port unchanged.
//
// ESM on purpose: imported by Vite for the web app and dynamically imported
// by the node test runner -- one implementation, no drift.

const TYPED = {
    float32: Float32Array,
    float64: Float64Array,
    int8:    Int8Array,
    uint8:   Uint8Array,
    int16:   Int16Array,
    uint16:  Uint16Array,
    int32:   Int32Array,
    int64:   BigInt64Array,
    float16: Uint16Array,     // ort represents f16 data as u16
    bool:    Uint8Array,
};

function typedView(dtype, bytes) {
    const T = TYPED[dtype];
    if (!T) throw new Error("nn provider: unsupported dtype '" + dtype + "'");
    // The Uint8Array is already a copy owned by us; view it in place.
    return new T(bytes.buffer, bytes.byteOffset, bytes.byteLength / T.BYTES_PER_ELEMENT);
}

// ORT metadata shapes carry symbolic dynamic dims (e.g. "N"); Roxal's
// contract, matching native ORT, is -1 for a dynamic dimension.
function normalizeShape(shape) {
    return (shape || []).map(d => (typeof d === 'number' && Number.isFinite(d)) ? d : -1);
}

function ioDescriptors(meta) {
    return (meta || []).map(m => ({
        name: m.name,
        shape: normalizeShape(m.shape),
        dtype: m.type,
    }));
}

export default function makeRoxalNN(ort, opts = {}) {
    const sessions = new Map();
    let nextId = 1;

    async function createWith(modelBytes, providers) {
        return ort.InferenceSession.create(modelBytes, { executionProviders: providers });
    }

    return {
        async create(modelBytes, device) {
            let session = null;
            let resolved = null;
            if (device === 'webgpu') {
                if (!opts.webgpu)
                    throw new Error('webgpu is not available in this host');
                session = await createWith(modelBytes, ['webgpu']);
                resolved = 'webgpu';
            } else if (device === 'cpu') {
                session = await createWith(modelBytes, ['wasm']);
                resolved = 'cpu';
            } else {                                     // 'auto'
                if (opts.webgpu) {
                    try {
                        session = await createWith(modelBytes, ['webgpu']);
                        resolved = 'webgpu';
                    } catch (e) {
                        // fall through to the wasm EP -- auto means "best available"
                    }
                }
                if (!session) {
                    session = await createWith(modelBytes, ['wasm']);
                    resolved = 'cpu';
                }
            }

            const id = nextId++;
            sessions.set(id, session);
            return {
                id,
                device: resolved,
                inputs: ioDescriptors(session.inputMetadata),
                outputs: ioDescriptors(session.outputMetadata),
            };
        },

        async run(id, feeds) {
            const session = sessions.get(id);
            if (!session) throw new Error('nn provider: unknown session ' + id);
            const feedMap = {};
            for (const f of feeds)
                feedMap[f.name] = new ort.Tensor(f.dtype, typedView(f.dtype, f.data), f.shape);
            const results = await session.run(feedMap);
            // Positional parity with native: order by the session's own list.
            return session.outputNames.map(name => {
                const t = results[name];
                const data = t.data;
                return {
                    dtype: t.type,
                    shape: normalizeShape(t.dims),
                    data: new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
                };
            });
        },

        close(id) {
            const session = sessions.get(id);
            if (session) {
                sessions.delete(id);
                session.release().catch(() => {});
            }
        },
    };
}
