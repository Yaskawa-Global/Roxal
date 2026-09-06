// DebugStoreAdapter: the IDE side of the `debug` store modules/web.rox
// exposes under a host-armed session.
//
// The store is re-exposed with a FRESH control actor at every script start
// (host-constructed capability), so the adapter never caches the store
// handle -- it re-acquires per call.  All calls are plain roxalStore RPC;
// no stop logic lives in JavaScript.

export function armDebugSession(rox, on) {
    rox.debugSession(on);
}

export class DebugStoreAdapter {
    constructor(rox) {
        this.rox = rox;
    }

    // Fresh handle every use: each run replaces the store (generation bump).
    store() {
        return this.rox.roxalStore('debug');
    }

    call(method, ...args) {
        return this.store().call(method, ...args);
    }

    async arm() { return this.call('arm', true); }

    // `file` is the display name the IDE submits sources under; the runtime
    // binds it against the synthetic "name#hash" registration.
    async setBreakpoints(file, lines) {
        return this.call('set_breakpoints', file, [...lines].sort((a, b) => a - b));
    }

    async state()  { return this.call('state'); }
    async pause()  { return this.call('pause'); }
    async resume() { return this.call('resume'); }
    async step(threadId, mode) { return this.call('step', threadId, mode); }
    async stack(threadId) { return this.call('stack', threadId); }
    async scopes(frameId) { return this.call('scopes', frameId); }
    async variables(ref, start = 0, count = 0) {
        return this.call('variables', ref, start, count);
    }
    async evaluate(frameId, expr) { return this.call('evaluate', frameId, expr); }

    // The stop summary the panel renders: stack of the selected thread plus
    // the top frame's local variables.  One round of bounded calls; any
    // failure returns null (the caller treats it as "not inspectable yet").
    async describeStop(state) {
        try {
            const stack = await this.stack(state.thread_id);
            if (!stack.ok || !stack.frames.length) return null;
            const scopes = await this.scopes(stack.frames[0].id);
            const locals = scopes.ok && scopes.scopes.find(s => s.name === 'Locals');
            let vars = [];
            if (locals) {
                const v = await this.variables(locals.ref);
                if (v.ok) vars = v.variables;
            }
            return { frames: stack.frames, vars };
        } catch {
            return null;
        }
    }
}
