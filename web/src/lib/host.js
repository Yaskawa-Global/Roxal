// Where the VM lives, from the page's point of view.
//
// Two hosts, one interface. WasmHost wraps the Emscripten module: the VM is
// a Worker in this tab, driven through its C entry points, with the store
// bridge in wasm/roxal-bridge.js. SocketHost speaks to `roxal --web-host`,
// a native VM in a local process, over a WebSocket, with the same store
// bridge decoded here from messages. Everything above this file -- the store
// contract, the React adapter, the IDE -- sees only the interface:
//
//   submit(source, name)            queue a script; never blocks
//   requestStop()                   ask a web.serve() park to return
//   interrupt()                     end the running script (clean VM exit)
//   completedCount() / lastResult() how many scripts have ended, last rc
//   config(key, value)              runtime switches (gc.disabled, ...)
//   debugSession/debugArm/setBreakpoints
//   roxalStore(name) / roxalStoreNames() / roxalStoreGeneration(name)
//   features                        the VM's compiled feature list, if known
//
// The wire codec and the store registry are the shared plain script
// wasm/roxal-wire.js (also linked into the wasm glue); importing it for its
// side effect publishes globalThis.RoxalWire.

import '../../../wasm/roxal-wire.js';

const W = globalThis.RoxalWire;

export class WasmHost {
    constructor(module) {
        this.kind = 'wasm';
        this.m = module;
        this.features = null;           // not known until the build store arrives
    }
    submit(source, name) {
        this.m.ccall('roxal_submit_source', null, ['string', 'string'], [source, name]);
    }
    requestStop() { this.m.ccall('roxal_request_stop', null, [], []); }
    interrupt() { this.m.ccall('roxal_interrupt_script', null, [], []); }
    completedCount() { return this.m.ccall('roxal_completed_count', 'number', [], []); }
    lastResult() { return this.m.ccall('roxal_last_result', 'number', [], []); }
    config(key, value) {
        this.m.ccall('roxal_config', null, ['string', 'string'], [key, String(value)]);
    }
    debugSession(on) { this.m.ccall('roxal_debug_session', null, ['number'], [on ? 1 : 0]); }
    debugArm(stopOnFatal) { this.m.ccall('roxal_debug_arm', null, ['number'], [stopOnFatal ? 1 : 0]); }
    setBreakpoints(file, lines) {
        this.m.ccall('roxal_debug_set_breakpoints', null, ['string', 'string'],
                     [file, [...lines].join(',')]);
    }
    roxalStore(name) { return this.m.roxalStore(name); }
    roxalStoreNames() { return this.m.roxalStoreNames(); }
    roxalStoreGeneration(name) { return this.m.roxalStoreGeneration?.(name) ?? 0; }
}

// Message framing, mirroring compiler/web/SocketHost.h:
//   host -> page   [u8 0][ops batch] | [u8 1][Str type][fields...]
//   page -> host   [u8 0][u8 inKind][u32 id][Str name][Str member][args]
//                  [u8 1][Str type][fields...]
const MSG_OPS = 0, MSG_CONTROL = 1;

export class SocketHost {
    constructor(url) {
        this.kind = 'socket';
        this.url = url;
        this.features = null;
        this.root = null;               // the host's --root, where files live
        this.stdlib = null;
        this.completed = 0;
        this.result = 0;
        this.running = false;
        this.outputListeners = new Set();
        this.closeListeners = new Set();
        this.stores = W.makeStores((kind, id, name, member, bytes) => {
            const w = new W.Writer();
            w.u8(MSG_OPS);
            w.u8(kind); w.u32(id);
            w.str(W.TAG_STR, name || ''); w.str(W.TAG_STR, member || '');
            w.raw(bytes);
            this.sendRaw(w.toUint8Array());
        });
    }

    // Resolves with the hello (version, features, root, ...) once connected.
    connect() {
        return new Promise((resolve, reject) => {
            let settled = false;
            const ws = new WebSocket(this.url);
            ws.binaryType = 'arraybuffer';
            this.ws = ws;
            ws.onerror = () => {
                if (!settled) { settled = true; reject(new Error('cannot reach the Roxal web host at ' + this.url
                    + ' — start it with `roxal --web-host` (see web/README.md)')); }
            };
            ws.onclose = () => {
                this.stores.rejectAll('the connection to the Roxal web host closed');
                for (const fn of this.closeListeners) fn();
                if (!settled) { settled = true; reject(new Error('the Roxal web host closed the connection')); }
            };
            ws.onmessage = ev => {
                const hello = this.onMessage(new Uint8Array(ev.data));
                if (hello && !settled) { settled = true; resolve(hello); }
            };
        });
    }

    onOutput(fn) { this.outputListeners.add(fn); return () => this.outputListeners.delete(fn); }
    onClose(fn) { this.closeListeners.add(fn); return () => this.closeListeners.delete(fn); }

    sendRaw(bytes) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(bytes);
    }
    control(type, fields = []) {
        const w = new W.Writer();
        w.u8(MSG_CONTROL);
        w.str(W.TAG_STR, type);
        for (const f of fields) {
            if (typeof f === 'string') w.str(W.TAG_STR, f);
            else w.u8(f ? 1 : 0);
        }
        this.sendRaw(w.toUint8Array());
    }

    // Returns the hello when this message was it, else undefined.
    onMessage(bytes) {
        const r = new W.Reader(bytes);
        const kind = r.u8();
        if (kind === MSG_OPS) {
            while (!r.atEnd()) {
                const op = r.u8();
                if (op === W.OP_STORE_DEFINE) this.stores.define(r);
                else if (op === W.OP_STORE_PATCH) this.stores.patch(r);
                else if (op === W.OP_STORE_RESOLVE) this.stores.resolve(r);
                else throw new Error('unexpected op ' + op + ' from the native host');
            }
            return undefined;
        }
        r.u8();                                   // Str tag
        const type = r.str();
        if (type === 'hello') {
            r.u8(); const version = r.str();
            r.u8(); const features = r.str().split(',').filter(Boolean);
            r.u8(); const root = r.str();
            r.u8(); const stdlib = r.str();
            this.completed = r.u32();
            this.result = r.u32();
            this.running = r.u32() !== 0;
            this.features = features;
            this.root = root;
            this.stdlib = stdlib;
            return { version, features, root, stdlib, completed: this.completed, running: this.running };
        }
        if (type === 'ended') {
            this.result = r.u32();
            this.completed = r.u32();
            this.running = false;
            return undefined;
        }
        if (type === 'output') {
            const isErr = r.u8() !== 0;
            r.u8(); const text = r.str();
            for (const fn of this.outputListeners) fn(text, isErr);
            return undefined;
        }
        console.warn('roxal web host: unknown control message', type);
        return undefined;
    }

    submit(source, name) { this.running = true; this.control('submit', [name, source]); }
    requestStop() { this.control('stop'); }
    interrupt() { this.control('interrupt'); }
    quit() { this.control('quit'); }
    completedCount() { return this.completed; }
    lastResult() { return this.result; }
    config(key, value) { this.control('config', [key, String(value)]); }
    debugSession(on) { this.control('debug_session', [on]); }
    debugArm(stopOnFatal) { this.control('debug_arm', [stopOnFatal]); }
    setBreakpoints(file, lines) { this.control('breakpoints', [file, [...lines].join(',')]); }
    roxalStore(name) { return this.stores.handle(name); }
    roxalStoreNames() { return this.stores.names(); }
    roxalStoreGeneration(name) { return this.stores.generation(name); }
}

// Which host a page should use: `?host=ws://...` selects a native host;
// otherwise the wasm VM in this tab.
export function hostUrlFromLocation() {
    try {
        const h = new URLSearchParams(location.search).get('host');
        return h && h.trim() ? h.trim() : null;
    } catch {
        return null;
    }
}
