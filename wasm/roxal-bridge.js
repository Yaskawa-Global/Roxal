// Main-thread half of the Roxal <-> JavaScript bridge.
//
// Linked with --pre-js, so it runs in the Emscripten glue scope and installs
// Module.roxalBridge before any wasm code executes. The VM (on a Worker) reaches
// exec() through MAIN_THREAD_EM_ASM, which proxies the call here and blocks the
// Worker until it returns.
//
// This file owns the handle table: every JS value Roxal holds lives here, and
// Roxal holds only int32 indices into it. Nothing else in the system may touch
// the DOM.
//
// The wire format mirrors compiler/web/JsBridge.h. Keep the two in step.

'use strict';

(function () {
    // ------------------------------------------------------------ wire format
    const TAG_NIL = 0, TAG_FALSE = 1, TAG_TRUE = 2, TAG_INT = 3, TAG_REAL = 4,
          TAG_STR = 5, TAG_HANDLE = 6, TAG_LIST = 7, TAG_DICT = 8, TAG_FUNC = 9,
          TAG_BYTES = 10, TAG_ERROR = 11, TAG_METHOD = 12;

    const OP_GLOBAL = 0, OP_GET = 1, OP_SET = 2, OP_CALL = 3, OP_INDEX = 4,
          OP_SETINDEX = 5, OP_NEW = 6, OP_RELEASE = 7, OP_LISTEN = 8,
          OP_UNLISTEN = 9, OP_TYPEOF = 10,
          OP_STORE_DEFINE = 11, OP_STORE_PATCH = 12, OP_STORE_RESOLVE = 13,
          OP_NN_REQUEST = 14;

    // Inbound kinds (must match roxal::web::Inbound).
    const IN_CALLBACK = 0, IN_STORE_CALL = 1, IN_STORE_WRITE = 2, IN_NN_RESULT = 3;

    // -------------------------------------------------------- handle table
    // Index 0 is permanently null so a zero handle needs no special case on
    // either side. Freed slots are recycled through `free` to keep the table
    // from growing without bound in a long-running UI.
    const table = [null];
    const free = [];

    function keep(v) {
        if (v === null || v === undefined) return 0;
        if (free.length) { const h = free.pop(); table[h] = v; return h; }
        return table.push(v) - 1;
    }
    function deref(h) {
        if (h === 0) return null;
        if (h >= table.length || table[h] === undefined)
            throw new Error('stale JS handle ' + h + ' (already released?)');
        return table[h];
    }
    function release(h) {
        if (h === 0 || h >= table.length) return;
        table[h] = undefined;
        free.push(h);
    }

    // ------------------------------------------------------------- decoding
    // Reads directly out of the wasm heap. Memory is shared with the Worker, so
    // there is no copy here -- that is the main reason the protocol is binary
    // rather than JSON.
    function Reader(ptr, len) {
        this.view = new DataView(HEAPU8.buffer, ptr, len);
        this.pos = 0;
    }
    Reader.prototype.u8 = function () { return this.view.getUint8(this.pos++); };
    Reader.prototype.u32 = function () {
        const v = this.view.getUint32(this.pos, true); this.pos += 4; return v;
    };
    Reader.prototype.f64 = function () {
        const v = this.view.getFloat64(this.pos, true); this.pos += 8; return v;
    };
    Reader.prototype.str = function () {
        const len = this.u32();
        const start = this.view.byteOffset + this.pos;
        this.pos += len;
        return UTF8ArrayToString(HEAPU8, start, len);
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
            case TAG_HANDLE: return deref(this.u32());
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
            case TAG_FUNC:   return makeCallback(this.u32());
            case TAG_BYTES: {
                const len = this.u32();
                const start = this.view.byteOffset + this.pos;
                this.pos += len;
                return HEAPU8.subarray(start, start + len);
            }
            default: throw new Error('unknown value tag');
        }
    };

    // ------------------------------------------------------------- encoding
    function Writer() { this.bytes = []; }
    Writer.prototype.u8 = function (v) { this.bytes.push(v & 0xff); };
    Writer.prototype.u32 = function (v) {
        this.bytes.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff);
    };
    Writer.prototype.str = function (tag, s) {
        const utf8 = new TextEncoder().encode(s);
        this.u8(tag);
        this.u32(utf8.length);
        for (let i = 0; i < utf8.length; i++) this.bytes.push(utf8[i]);
    };
    Writer.prototype.value = function (v) {
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
                    for (let i = 0; i < 8; i++) this.bytes.push(b[i]);
                }
                return;
            case 'string': this.str(TAG_STR, v); return;
            default: {
                // Plain data goes across BY VALUE; anything with identity or
                // behaviour goes by reference as a handle. Getting this wrong is
                // not subtle -- an event payload arriving as an opaque handle
                // cannot be indexed, and a DOM node arriving as a dict would be
                // a dead copy.
                if (v instanceof Uint8Array) {
                    this.u8(TAG_BYTES);
                    this.u32(v.length);
                    for (let i = 0; i < v.length; i++) this.bytes.push(v[i]);
                    return;
                }
                if (Array.isArray(v)) {
                    this.u8(TAG_LIST);
                    this.u32(v.length);
                    for (let i = 0; i < v.length; i++) this.value(v[i]);
                    return;
                }
                const proto = Object.getPrototypeOf(v);
                if (typeof v === 'object' && (proto === Object.prototype || proto === null)) {
                    const keys = Object.keys(v);
                    this.u8(TAG_DICT);
                    this.u32(keys.length);
                    for (const k of keys) { this.str(TAG_STR, k); this.value(v[k]); }
                    return;
                }
                this.u8(TAG_HANDLE); this.u32(keep(v));
                return;
            }
        }
    };
    Writer.prototype.error = function (msg) {
        this.bytes.length = 0;
        this.str(TAG_ERROR, msg);
    };

    // Copy into a malloc'd block the C++ side owns and frees.
    // Layout: [u32 byteLen][payload].
    Writer.prototype.toWasm = function () {
        const n = this.bytes.length;
        const ptr = _malloc(4 + n);
        HEAPU8[ptr] = n & 0xff;
        HEAPU8[ptr + 1] = (n >> 8) & 0xff;
        HEAPU8[ptr + 2] = (n >> 16) & 0xff;
        HEAPU8[ptr + 3] = (n >>> 24) & 0xff;
        HEAPU8.set(this.bytes, ptr + 4);
        return ptr;
    };

    // ------------------------------------------------------------ callbacks
    // A Roxal callable passed to JS becomes a function that QUEUES an invocation
    // and returns immediately. It cannot return the Roxal result: waiting for the
    // VM would block the main thread, which is exactly what the architecture
    // forbids (and Atomics.wait is unavailable here anyway).
    // Post work to the VM thread. Never waits for a result -- the browser main
    // thread cannot Atomics.wait, so anything needing a reply gets a Promise that
    // the VM settles later via OP_STORE_RESOLVE.
    function postInbound(kind, id, name, member, encodedArgs) {
        const n = encodedArgs.length;
        const argPtr = _malloc(n || 1);
        if (n) HEAPU8.set(encodedArgs, argPtr);
        const namePtr = name ? stringToNewUTF8(name) : 0;
        const memberPtr = member ? stringToNewUTF8(member) : 0;
        try {
            _roxal_web_queue_inbound(kind, id, namePtr, memberPtr, argPtr, n);
        } finally {
            _free(argPtr);
            if (namePtr) _free(namePtr);
            if (memberPtr) _free(memberPtr);
        }
    }

    function makeCallback(id) {
        return function (arg) {
            const w = new Writer();
            try {
                w.value(arg === undefined ? null : arg);
            } catch (e) {
                w.bytes.length = 0;
                w.u8(TAG_NIL);
            }
            postInbound(IN_CALLBACK, id, null, null, w.bytes);
        };
    }

    // A DOM Event is a host object with a huge surface; handing the whole thing
    // over as a handle would make every field read a round trip. Send the fields
    // handlers actually use, plus a handle for the rest.
    function summariseEvent(ev) {
        const out = {
            type: ev.type,
            target: keepAsHandleMarker(ev.target),
        };
        if ('clientX' in ev) { out.x = ev.clientX; out.y = ev.clientY; }
        if ('key' in ev)     { out.key = ev.key; }
        if (ev.target && 'value' in ev.target) out.value = ev.target.value;
        if (ev.target && 'checked' in ev.target) out.checked = ev.target.checked;
        return out;
    }
    // Writer.value() turns a non-primitive into a handle, so passing the node
    // through unchanged is enough -- this exists to make that intent obvious.
    function keepAsHandleMarker(node) { return node; }

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
    const stores = new Map();
    let defineSeq = 0;                // monotonic across ALL stores (see OP_STORE_DEFINE)
    let nextCallId = 1;
    const CALL_TIMEOUT_MS = 20000;
    const pendingCalls = new Map();   // callId -> {resolve, reject}

    function storeRecord(name) {
        let rec = stores.get(name);
        if (!rec) {
            rec = { name, snapshot: Object.freeze({}), methods: [], subs: new Set(), queued: false };
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

    function storeHandle(name) {
        const rec = storeRecord(name);
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
                    for (const a of args) w.value(a);
                    postInbound(IN_STORE_CALL, id, name, method, w.bytes);
                });
            },
            set(prop, value) {
                const w = new Writer();
                w.value(value);
                postInbound(IN_STORE_WRITE, 0, name, prop, w.bytes);
            },
        };
    }

    // ------------------------------------------------------------- NN provider
    // ai.nn delegates inference to Module.roxalNN, registered by the host:
    //   create(modelBytes:Uint8Array, device) -> Promise<{id, device, inputs, outputs}>
    //   run(id, [{name,dtype,shape,data:Uint8Array}]) -> Promise<[{dtype,shape,data:Uint8Array}]>
    //   close(id)
    // The browser host plugs in onnxruntime-web (WebGPU EP with wasm fallback),
    // node plugs in the same package's wasm EP, an Electron host can plug in
    // onnxruntime-node -- one Roxal module, best engine per host. Replies go
    // back over the inbound queue as [ok, body]; a missing provider answers
    // every request with an error so Model() raises cleanly instead of hanging.
    function nnReply(callId, ok, body) {
        const w = new Writer();
        w.u8(TAG_LIST);
        w.u32(2);
        w.value(ok ? 1 : 0);
        try { w.value(body); }
        catch (e) { w.bytes.length = 0; w.u8(TAG_LIST); w.u32(2); w.value(0); w.value(String(e)); }
        postInbound(IN_NN_RESULT, callId, null, null, w.bytes);
    }
    function nnDispatch(callId, fn) {
        const p = Module.roxalNN;
        if (!p) {
            nnReply(callId, false,
                    'this host provides no NN backend (Module.roxalNN is not registered)');
            return;
        }
        Promise.resolve().then(() => fn(p)).then(
            body => nnReply(callId, true, body),
            err  => nnReply(callId, false, String((err && err.message) || err)));
    }

    // ---------------------------------------------------------------- ops
    function runOne(r, w) {
        const op = r.u8();
        switch (op) {
            case OP_GLOBAL: {
                const name = (r.u8(), r.str());   // consume the Str tag
                // globalThis rather than window: the same code then works in a
                // browser, a worker and node, and "window" stays available as an
                // alias for the page-facing name Roxal users expect.
                const g = (name === 'window' || name === 'globalThis')
                        ? globalThis : globalThis[name];
                if (g === undefined) throw new Error("no global '" + name + "'");
                w.value(g);
                return true;
            }
            case OP_GET: {
                const h = r.u32();
                const obj = deref(h);
                const name = (r.u8(), r.str());
                const v = obj[name];
                // A function property becomes a BOUND callable, not a bare
                // handle: Roxal compiles obj.method(args) to a property read
                // followed by a call, so the read has to produce something
                // callable that still remembers its receiver. A fresh handle
                // gives the binding its own reference to keep alive.
                if (typeof v === 'function') {
                    w.u8(TAG_METHOD);
                    w.u32(keep(obj));
                    w.str(TAG_STR, name);
                } else {
                    w.value(v);
                }
                return true;
            }
            case OP_SET: {
                const obj = deref(r.u32());
                const name = (r.u8(), r.str());
                obj[name] = r.value();
                return false;
            }
            case OP_CALL: {
                const obj = deref(r.u32());
                const name = (r.u8(), r.str());
                const argc = r.u32();
                const args = new Array(argc);
                for (let i = 0; i < argc; i++) args[i] = r.value();
                const fn = obj[name];
                if (typeof fn !== 'function')
                    throw new Error("'" + name + "' is not a function");
                w.value(fn.apply(obj, args));
                return true;
            }
            case OP_INDEX: {
                const obj = deref(r.u32());
                w.value(obj[r.value()]);
                return true;
            }
            case OP_SETINDEX: {
                const obj = deref(r.u32());
                const idx = r.value();
                obj[idx] = r.value();
                return false;
            }
            case OP_NEW: {
                const ctor = deref(r.u32());
                const argc = r.u32();
                const args = new Array(argc);
                for (let i = 0; i < argc; i++) args[i] = r.value();
                w.value(new (Function.prototype.bind.apply(ctor, [null].concat(args)))());
                return true;
            }
            case OP_RELEASE: {
                release(r.u32());
                return false;
            }
            case OP_LISTEN: {
                const target = deref(r.u32());
                const event = (r.u8(), r.str());
                const cbId = r.u32();
                const raw = makeCallback(cbId);
                const handler = function (ev) { raw(summariseEvent(ev)); };
                target.addEventListener(event, handler);
                // The handle keeps enough to unsubscribe later.
                w.value({ __roxalListener: true, target: target, event: event, handler: handler });
                return true;
            }
            case OP_UNLISTEN: {
                const h = r.u32();
                const rec = deref(h);
                if (rec && rec.__roxalListener)
                    rec.target.removeEventListener(rec.event, rec.handler);
                release(h);
                return false;
            }
            case OP_STORE_DEFINE: {
                const name = (r.u8(), r.str());
                const snapshot = r.value();
                const methods = r.value();
                const rec = storeRecord(name);
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
                w.value(null);
                return true;
            }
            case OP_STORE_PATCH: {
                const name = (r.u8(), r.str());
                applyPatch(storeRecord(name), r.value() || {});
                return false;
            }
            case OP_STORE_RESOLVE: {
                const id = r.u32();
                const result = r.value();
                const error = r.value();
                const p = pendingCalls.get(id);
                pendingCalls.delete(id);
                if (p) {
                    if (error) p.reject(new Error(String(error)));
                    else p.resolve(result);
                }
                return false;
            }
            case OP_NN_REQUEST: {
                const callId = r.u32();
                const kind = r.u8();
                if (kind === 0) {                       // create
                    // r.value() for TAG_BYTES is a VIEW over the wasm heap --
                    // valid only during this synchronous op (the heap can grow
                    // and move). Copy before anything async touches it.
                    const model = new Uint8Array(r.value());
                    const device = (r.u8(), r.str());
                    nnDispatch(callId, p => p.create(model, device));
                } else if (kind === 1) {                // run
                    const session = r.u32();
                    const n = r.u32();
                    const feeds = [];
                    for (let i = 0; i < n; i++) {
                        const name = (r.u8(), r.str());
                        const dtype = (r.u8(), r.str());
                        const shape = r.value();
                        const data = new Uint8Array(r.value());   // copy, as above
                        feeds.push({ name, dtype, shape, data });
                    }
                    nnDispatch(callId, p => p.run(session, feeds));
                } else {                                // close: fire-and-forget
                    const session = r.u32();
                    const p = Module.roxalNN;
                    if (p) Promise.resolve().then(() => p.close(session)).catch(() => {});
                }
                return false;                           // reply arrives inbound
            }
            case OP_TYPEOF: {
                const v = deref(r.u32());
                w.str(TAG_STR, v === null ? 'null' : typeof v);
                return true;
            }
            default:
                throw new Error('unknown op ' + op);
        }
    }

    // Execute a batch. Only the final operation's value is returned; deferred
    // batches produce nothing and return 0 (a null pointer) so the caller has
    // nothing to free.
    function exec(ptr, len) {
        const r = new Reader(ptr, len);
        const w = new Writer();
        let produced = false;
        try {
            while (!r.atEnd()) {
                w.bytes.length = 0;          // only the last op's value survives
                produced = runOne(r, w);
            }
        } catch (e) {
            w.error(String((e && e.message) || e));
            return w.toWasm();
        }
        return produced ? w.toWasm() : 0;
    }

    Module.roxalBridge = { exec: exec, table: table, keep: keep, deref: deref };

    // Public API for app code and framework adapters.
    Module.roxalStore = storeHandle;
    Module.roxalStoreNames = () => Array.from(stores.keys());
    Module.roxalStoreGeneration = name => stores.get(name)?.generation || 0;
})();
