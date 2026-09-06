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
    // The codec and the store registry are shared with the socket host:
    // wasm/roxal-wire.js, linked just before this file.
    const W = globalThis.RoxalWire;
    const { TAG_STR, TAG_METHOD, TAG_LIST, TAG_NIL,
            OP_GLOBAL, OP_GET, OP_SET, OP_CALL, OP_INDEX, OP_SETINDEX, OP_NEW,
            OP_RELEASE, OP_LISTEN, OP_UNLISTEN, OP_TYPEOF, OP_STORE_DEFINE,
            OP_STORE_PATCH, OP_STORE_RESOLVE, OP_NN_REQUEST, OP_UNICODE_CASE,
            IN_CALLBACK, IN_NN_RESULT } = W;

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

    // ------------------------------------------------------------- codec
    // Reads directly out of the wasm heap. Memory is shared with the Worker, so
    // there is no copy here -- that is the main reason the protocol is binary
    // rather than JSON. Handles and callables are resolved through this file's
    // table and registry.
    // copyPayloads: tensor bytes are copied out of the heap (see roxal-wire)
    const readerHooks = { deref, makeCallback, copyPayloads: true };
    function Reader(ptr, len) {
        return new W.Reader(HEAPU8.subarray(ptr, ptr + len), readerHooks);
    }
    function Writer() {
        const w = new W.Writer();
        // By-reference values go through the handle table.
        const value = w.value;
        w.value = v => value.call(w, v, keep);
        // Copy into a malloc'd block the C++ side owns and frees.
        // Layout: [u32 byteLen][payload].
        w.toWasm = function () {
            const n = this.bytes.length;
            const ptr = _malloc(4 + n);
            HEAPU8[ptr] = n & 0xff;
            HEAPU8[ptr + 1] = (n >> 8) & 0xff;
            HEAPU8[ptr + 2] = (n >> 16) & 0xff;
            HEAPU8[ptr + 3] = (n >>> 24) & 0xff;
            HEAPU8.set(this.bytes, ptr + 4);
            return ptr;
        };
        return w;
    }

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
    // The registry is shared code; this host posts inbound work through the
    // C entry point and keeps by-reference values in its handle table.
    const stores = W.makeStores(postInbound, keep);

    // ------------------------------------------------------------ unicode
    // Title case, matching ICU's toTitle: each WORD's first letter is
    // upper-cased and the rest lower-cased. Word boundaries come from
    // Intl.Segmenter, which implements the same UAX #29 rules ICU does -- a
    // regex on letters would disagree with the native build on exactly the
    // interesting cases ("multi-word" is two words, "don't" is one).
    const wordSegmenter = typeof Intl !== 'undefined' && Intl.Segmenter
        ? new Intl.Segmenter(undefined, { granularity: 'word' }) : null;

    function titleCase(text) {
        if (!wordSegmenter) {
            // No Intl.Segmenter (very old engine): fall back to ASCII-ish word
            // splitting rather than refusing -- still better than the native
            // build's alternative here, which is not running at all.
            return text.toLowerCase().replace(/(^|\P{L})(\p{L})/gu,
                                              (_, sep, ch) => sep + ch.toUpperCase());
        }
        let out = '';
        for (const { segment, isWordLike } of wordSegmenter.segment(text)) {
            out += isWordLike
                ? segment[0].toUpperCase() + segment.slice(1).toLowerCase()
                : segment;
        }
        return out;
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
            case OP_STORE_DEFINE:
                stores.define(r);
                return false;
            case OP_STORE_PATCH:
                stores.patch(r);
                return false;
            case OP_STORE_RESOLVE:
                stores.resolve(r);
                return false;
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
            case OP_UNICODE_CASE: {
                const mode = r.u8();
                const text = (r.u8(), r.str());
                w.str(TAG_STR, mode === 2 ? titleCase(text)
                             : mode === 1 ? text.toUpperCase()
                                          : text.toLowerCase());
                return true;
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
    Module.roxalStore = stores.handle;
    Module.roxalStoreNames = stores.names;
    Module.roxalStoreGeneration = stores.generation;
})();
