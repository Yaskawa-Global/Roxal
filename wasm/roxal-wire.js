// The JavaScript half of Roxal's web wire format, shared by every host.
//
// Two hosts speak it: the wasm bridge (wasm/roxal-bridge.js, linked into the
// Emscripten glue as a --pre-js, where the VM is a Worker and batches arrive
// as pointers into the shared heap) and the socket host (web/src/lib/host.js,
// where the VM is a native process and batches arrive as WebSocket messages).
// Everything host-specific -- how bytes arrive, how inbound work is posted --
// is parameterised; this file owns only the codec and the store registry.
//
// It is a plain script, not a module: it must load inside the Emscripten
// factory scope as well as via an ES import and via `new Function` in node.
// It publishes itself as globalThis.RoxalWire.
//
// The wire format mirrors compiler/web/JsBridge.h. Keep the two in step.

(function () {
    'use strict';

    // ------------------------------------------------------------ wire format
    const TAG_NIL = 0, TAG_FALSE = 1, TAG_TRUE = 2, TAG_INT = 3, TAG_REAL = 4,
          TAG_STR = 5, TAG_HANDLE = 6, TAG_LIST = 7, TAG_DICT = 8, TAG_FUNC = 9,
          TAG_BYTES = 10, TAG_ERROR = 11, TAG_METHOD = 12, TAG_TENSOR = 13;

    const OP_GLOBAL = 0, OP_GET = 1, OP_SET = 2, OP_CALL = 3, OP_INDEX = 4,
          OP_SETINDEX = 5, OP_NEW = 6, OP_RELEASE = 7, OP_LISTEN = 8,
          OP_UNLISTEN = 9, OP_TYPEOF = 10,
          OP_STORE_DEFINE = 11, OP_STORE_PATCH = 12, OP_STORE_RESOLVE = 13,
          OP_NN_REQUEST = 14, OP_UNICODE_CASE = 15;

    // Inbound kinds (must match roxal::web::Inbound).
    const IN_CALLBACK = 0, IN_STORE_CALL = 1, IN_STORE_WRITE = 2, IN_NN_RESULT = 3;

    const utf8Decoder = new TextDecoder();
    const utf8Encoder = new TextEncoder();

    // dtype name -> typed array constructor, for tensors arriving by value.
    const TYPED = {
        float32: Float32Array, float64: Float64Array,
        int8: Int8Array, int16: Int16Array, int32: Int32Array,
        uint8: Uint8Array, uint16: Uint16Array,
        int64: (typeof BigInt64Array !== 'undefined') ? BigInt64Array : null,
        float16: Uint16Array,          // raw halves; no JS half type
        bool: Uint8Array,
    };

    // A tensor by value. `data` is a typed array over the payload bytes -- a
    // VIEW for a wasm-heap reader (valid only during the synchronous op that
    // produced it) and a copy-free view of the message for a socket reader.
    class Tensor {
        constructor(dtype, shape, data) {
            this.dtype = dtype;
            this.shape = shape;
            this.data = data;
        }
        get numel() { return this.shape.reduce((a, b) => a * b, 1); }
    }

    // ------------------------------------------------------------- decoding
    // Reads a batch out of a Uint8Array. `hooks` supply what only a host can
    // resolve: a JS handle table (wasm) and Roxal callables (wasm). A host
    // without them leaves the hooks out and a stray handle throws.
    function Reader(bytes, hooks) {
        this.u8s = bytes;
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        this.pos = 0;
        this.hooks = hooks || {};
    }
    Reader.prototype.u8 = function () { return this.view.getUint8(this.pos++); };
    Reader.prototype.u32 = function () {
        const v = this.view.getUint32(this.pos, true); this.pos += 4; return v;
    };
    Reader.prototype.f64 = function () {
        const v = this.view.getFloat64(this.pos, true); this.pos += 8; return v;
    };
    Reader.prototype.bytes = function (len) {
        const start = this.pos;
        this.pos += len;
        return this.u8s.subarray(start, start + len);
    };
    Reader.prototype.str = function () {
        const len = this.u32();
        const span = this.bytes(len);
        // TextDecoder refuses SharedArrayBuffer-backed views (the wasm heap):
        // copy those first. Strings are short; the copy is cheaper than the
        // decoding either way.
        const shared = typeof SharedArrayBuffer !== 'undefined'
                    && span.buffer instanceof SharedArrayBuffer;
        return utf8Decoder.decode(shared ? span.slice() : span);
    };
    Reader.prototype.atEnd = function () { return this.pos >= this.view.byteLength; };

    Reader.prototype.value = function () {
        switch (this.u8()) {
            case TAG_NIL:    return null;
            case TAG_FALSE:  return false;
            case TAG_TRUE:   return true;
            case TAG_INT:    return this.view.getInt32((this.pos += 4) - 4, true);
            case TAG_REAL:   return this.f64();
            case TAG_STR:    return this.str();
            case TAG_HANDLE: {
                const h = this.u32();
                if (!this.hooks.deref) throw new Error('JS handles do not exist on this host');
                return this.hooks.deref(h);
            }
            case TAG_LIST: {
                const n = this.u32(), out = new Array(n);
                for (let i = 0; i < n; i++) out[i] = this.value();
                return out;
            }
            case TAG_DICT: {
                const n = this.u32(), out = {};
                for (let i = 0; i < n; i++) {
                    if (this.u8() !== TAG_STR) throw new Error('malformed dict key');
                    const k = this.str();
                    out[k] = this.value();
                }
                return out;
            }
            case TAG_FUNC: {
                const id = this.u32();
                if (!this.hooks.makeCallback) throw new Error('Roxal callables cannot cross to this host');
                return this.hooks.makeCallback(id);
            }
            case TAG_BYTES: {
                const b = this.bytes(this.u32());
                return this.hooks.copyPayloads ? b.slice() : b;   // (see TAG_TENSOR)
            }
            case TAG_TENSOR: {
                if (this.u8() !== TAG_STR) throw new Error('malformed tensor dtype');
                const dtype = this.str();
                const ndim = this.u32();
                const shape = new Array(ndim);
                for (let i = 0; i < ndim; i++) shape[i] = this.u32();
                const raw = this.bytes(this.u32());
                const ctor = TYPED[dtype];
                if (!ctor) throw new Error('unsupported tensor dtype ' + dtype);
                // A typed array needs its element alignment; the payload sits
                // at an arbitrary offset inside the message, so copy when
                // misaligned (a Uint8Array view is always fine).  A reader
                // over the wasm heap (hooks.copyPayloads) always copies: the
                // bytes belong to a bridge batch the C++ side reuses, and
                // memory growth would detach a view -- a store keeps the
                // tensor long after the batch is gone.
                const elem = ctor.BYTES_PER_ELEMENT;
                const data = (raw.byteOffset % elem === 0 && !this.hooks.copyPayloads)
                    ? new ctor(raw.buffer, raw.byteOffset, raw.byteLength / elem)
                    : new ctor(raw.slice().buffer);
                return new Tensor(dtype, shape, data);
            }
            case TAG_ERROR: throw new Error(this.str());
            default: throw new Error('unknown value tag');
        }
    };

    // ------------------------------------------------------------- encoding
    function Writer() { this.bytes = []; }
    Writer.prototype.u8 = function (v) { this.bytes.push(v & 0xff); };
    Writer.prototype.u32 = function (v) {
        this.bytes.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff);
    };
    Writer.prototype.raw = function (u8s) {
        for (let i = 0; i < u8s.length; i++) this.bytes.push(u8s[i]);
    };
    Writer.prototype.str = function (tag, s) {
        const utf8 = utf8Encoder.encode(s);
        this.u8(tag);
        this.u32(utf8.length);
        this.raw(utf8);
    };
    // Encode `v`. Plain data goes across BY VALUE; anything with identity or
    // behaviour goes by reference through `keep` (a host's handle table) --
    // a host without one cannot pass such values and gets an error.
    Writer.prototype.value = function (v, keep) {
        if (v === null || v === undefined) { this.u8(TAG_NIL); return; }
        switch (typeof v) {
            case 'boolean': this.u8(v ? TAG_TRUE : TAG_FALSE); return;
            case 'number':
                if (Number.isInteger(v) && v >= -2147483648 && v <= 2147483647) {
                    this.u8(TAG_INT); this.u32(v | 0);
                } else {
                    this.u8(TAG_REAL);
                    const b = new Uint8Array(8);
                    new DataView(b.buffer).setFloat64(0, v, true);
                    this.raw(b);
                }
                return;
            case 'string': this.str(TAG_STR, v); return;
            default: {
                if (v instanceof Tensor) {
                    this.u8(TAG_TENSOR);
                    this.str(TAG_STR, v.dtype);
                    this.u32(v.shape.length);
                    for (const d of v.shape) this.u32(d);
                    const raw = new Uint8Array(v.data.buffer, v.data.byteOffset, v.data.byteLength);
                    this.u32(raw.length);
                    this.raw(raw);
                    return;
                }
                if (v instanceof Uint8Array) {
                    this.u8(TAG_BYTES);
                    this.u32(v.length);
                    this.raw(v);
                    return;
                }
                if (Array.isArray(v)) {
                    this.u8(TAG_LIST);
                    this.u32(v.length);
                    for (let i = 0; i < v.length; i++) this.value(v[i], keep);
                    return;
                }
                const proto = Object.getPrototypeOf(v);
                if (typeof v === 'object' && (proto === Object.prototype || proto === null)) {
                    const keys = Object.keys(v);
                    this.u8(TAG_DICT);
                    this.u32(keys.length);
                    for (const k of keys) { this.str(TAG_STR, k); this.value(v[k], keep); }
                    return;
                }
                if (!keep) throw new Error('cannot pass a ' + (v.constructor?.name || typeof v)
                                           + ' to Roxal on this host');
                this.u8(TAG_HANDLE); this.u32(keep(v));
                return;
            }
        }
    };
    Writer.prototype.error = function (msg) {
        this.bytes.length = 0;
        this.str(TAG_ERROR, msg);
    };
    Writer.prototype.toUint8Array = function () { return Uint8Array.from(this.bytes); };

    // ------------------------------------------------------------- stores
    // A store is the framework-agnostic contract every adapter builds on:
    //
    //     subscribe(fn) -> unsubscribe    fn receives the new snapshot
    //     getSnapshot() -> frozen object  STABLE identity until something changes
    //     call(method, ...args) -> Promise
    //     set(prop, value)
    //
    // getSnapshot's referential stability is the strict requirement: React's
    // useSyncExternalStore re-renders forever without it. Svelte and Vue only need
    // the pushed value, so satisfying React satisfies everyone.
    //
    // `post(kind, id, name, member, bytes)` is how this host sends inbound work
    // to the VM; `keep` (optional) is its handle table for by-reference values.
    function makeStores(post, keep) {
        const stores = new Map();
        let defineSeq = 0;                // monotonic across ALL stores (see define)
        // Call ids only have to be unique per connection. A socket client that
        // reconnects starts from a fresh base so a reply meant for the previous
        // page cannot settle one of its promises.
        let nextCallId = 1 + ((Date.now() % 100000) * 1000);
        const CALL_TIMEOUT_MS = 20000;
        const pendingCalls = new Map();   // callId -> {resolve, reject}

        function record(name) {
            let rec = stores.get(name);
            if (!rec) {
                rec = { name, snapshot: Object.freeze({}), methods: [], subs: new Set(),
                        queued: false, generation: 0 };
                stores.set(name, rec);
            }
            return rec;
        }

        // Replace the snapshot wholesale rather than mutating: subscribers compare by
        // identity, and a mutated object would look unchanged.
        function applyPatch(rec, delta) {
            rec.snapshot = Object.freeze(Object.assign({}, rec.snapshot, delta));
            notify(rec);
        }

        // Coalesce into a microtask, so several patches in one turn cause one render.
        function notify(rec) {
            if (rec.queued) return;
            rec.queued = true;
            Promise.resolve().then(() => {
                rec.queued = false;
                const snap = rec.snapshot;
                for (const fn of Array.from(rec.subs)) {
                    try { fn(snap); } catch (e) { console.error('roxal store subscriber threw:', e); }
                }
            });
        }

        function handle(name) {
            const rec = record(name);
            return {
                name,
                get methods() { return rec.methods.slice(); },
                subscribe(fn) {
                    rec.subs.add(fn);
                    return () => rec.subs.delete(fn);
                },
                getSnapshot() { return rec.snapshot; },
                call(method, ...args) {
                    return new Promise((resolve, reject) => {
                        const id = nextCallId++;
                        // A store call is only serviced while a script is RUNNING or
                        // PARKED: the host loop is pumped from the VM's dispatch
                        // loop. If the last script ended, nothing pumps and this
                        // promise would never settle -- a silently wedged UI, with
                        // no way for the caller to tell "slow" from "dead". Reject
                        // instead, generously enough not to catch a legitimately
                        // slow method (a big parse is ~0.5s).
                        const timer = setTimeout(() => {
                            if (!pendingCalls.delete(id)) return;
                            reject(new Error(
                                `roxal store call ${name}.${method}() was never serviced ` +
                                `(${CALL_TIMEOUT_MS}ms) — is a script still running? ` +
                                `A script that ends without web.serve() leaves nothing to pump.`));
                        }, CALL_TIMEOUT_MS);
                        pendingCalls.set(id, {
                            resolve: v => { clearTimeout(timer); resolve(v); },
                            reject:  e => { clearTimeout(timer); reject(e); },
                        });
                        const w = new Writer();
                        w.u8(TAG_LIST);
                        w.u32(args.length);
                        for (const a of args) w.value(a, keep);
                        post(IN_STORE_CALL, id, name, method, w.bytes);
                    });
                },
                set(prop, value) {
                    const w = new Writer();
                    w.value(value, keep);
                    post(IN_STORE_WRITE, 0, name, prop, w.bytes);
                },
            };
        }

        // The three store ops a batch can carry. Each takes a Reader positioned
        // just after the op byte.
        function define(r) {
            const name = (r.u8(), r.str());
            const snapshot = r.value();
            const methods = r.value();
            const rec = record(name);
            rec.methods = Array.isArray(methods) ? methods : [];
            rec.snapshot = Object.freeze(Object.assign({}, snapshot));
            // A GLOBAL define sequence, not a per-store count. Two things
            // depend on it: a harness tells a fresh store from the stale
            // record the registry keeps across runs (any increase does
            // that), and a host picks the store the RUNNING script just
            // exposed by taking the highest. Per-store counting broke the
            // second one -- a store defined twice outranked one defined
            // once no matter which script was live.
            rec.generation = ++defineSeq;
            notify(rec);
        }
        function patch(r) {
            const name = (r.u8(), r.str());
            applyPatch(record(name), r.value() || {});
        }
        function resolve(r) {
            const id = r.u32();
            const result = r.value();
            const error = r.value();
            const p = pendingCalls.get(id);
            pendingCalls.delete(id);
            if (p) {
                if (error) p.reject(new Error(String(error)));
                else p.resolve(result);
            }
        }
        // Settle everything outstanding -- the connection is gone.
        function rejectAll(reason) {
            for (const [id, p] of pendingCalls) { pendingCalls.delete(id); p.reject(new Error(reason)); }
        }

        return {
            handle,
            names: () => Array.from(stores.keys()),
            generation: name => stores.get(name)?.generation || 0,
            define, patch, resolve, rejectAll,
        };
    }

    globalThis.RoxalWire = {
        TAG_NIL, TAG_FALSE, TAG_TRUE, TAG_INT, TAG_REAL, TAG_STR, TAG_HANDLE, TAG_LIST,
        TAG_DICT, TAG_FUNC, TAG_BYTES, TAG_ERROR, TAG_METHOD, TAG_TENSOR,
        OP_GLOBAL, OP_GET, OP_SET, OP_CALL, OP_INDEX, OP_SETINDEX, OP_NEW, OP_RELEASE,
        OP_LISTEN, OP_UNLISTEN, OP_TYPEOF, OP_STORE_DEFINE, OP_STORE_PATCH,
        OP_STORE_RESOLVE, OP_NN_REQUEST, OP_UNICODE_CASE,
        IN_CALLBACK, IN_STORE_CALL, IN_STORE_WRITE, IN_NN_RESULT,
        Tensor, Reader, Writer, makeStores,
    };
})();
