// Native web host test: drives `roxal --web-host` over a WebSocket the way the
// browser page does, using the shared wire codec (roxal-wire.js) and node's
// built-in WebSocket client (node >= 22).
//
//   node wasm/test-sockethost.cjs [path/to/roxal]
//
// Checks the control channel (hello, submit, output, ended, stop, quit), the
// store bridge (define, patch, call, write), tensors in both directions, and
// that a second client displaces the first and receives the stores again.

const { spawn } = require('child_process');
const crypto = require('crypto');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');

const repo = path.resolve(__dirname, '..');
const roxalBin = process.argv[2] || path.join(repo, 'build', 'roxal');

new Function(fs.readFileSync(path.join(__dirname, 'roxal-wire.js'), 'utf8'))();
const W = globalThis.RoxalWire;

const results = [];
const check = (name, ok, detail) => results.push([name, !!ok, detail || '']);
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function waitFor(pred, ms, what) {
    const deadline = Date.now() + ms;
    while (!pred()) {
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(10);
    }
}

// ---------------------------------------------------------------- client
// A minimal socket client: exactly what web/src/lib/host.js does, without the
// browser. Kept independent so the test does not share bugs with the page.
class Client {
    constructor(url) {
        this.url = url;
        this.output = '';
        this.events = [];            // control messages, in order
        this.hello = null;
        this.stores = W.makeStores((kind, id, name, member, bytes) => {
            const w = new W.Writer();
            w.u8(0);                                  // ops message
            w.u8(kind); w.u32(id);
            w.str(W.TAG_STR, name || ''); w.str(W.TAG_STR, member || '');
            w.raw(bytes);
            this.ws.send(w.toUint8Array());
        });
    }
    connect() {
        return new Promise((resolve, reject) => {
            this.ws = new WebSocket(this.url);
            this.ws.binaryType = 'arraybuffer';
            this.ws.onopen = () => resolve();
            this.ws.onerror = e => reject(new Error('websocket error ' + (e.message || '')));
            this.ws.onclose = () => { this.closed = true; this.stores.rejectAll('connection closed'); };
            this.ws.onmessage = ev => this.onMessage(new Uint8Array(ev.data));
        });
    }
    onMessage(bytes) {
        const r = new W.Reader(bytes);
        const kind = r.u8();
        if (kind === 0) {
            while (!r.atEnd()) {
                const op = r.u8();
                if (op === W.OP_STORE_DEFINE) this.stores.define(r);
                else if (op === W.OP_STORE_PATCH) this.stores.patch(r);
                else if (op === W.OP_STORE_RESOLVE) this.stores.resolve(r);
                else throw new Error('unexpected op ' + op + ' from a native host');
            }
            return;
        }
        r.u8();                                       // Str tag
        const type = r.str();
        if (type === 'hello') {
            r.u8(); const version = r.str();
            r.u8(); const features = r.str().split(',');
            r.u8(); const root = r.str();
            r.u8(); const stdlib = r.str();
            const completed = r.u32(), lastResult = r.u32(), running = r.u32();
            this.hello = { version, features, root, stdlib, completed, lastResult, running };
        } else if (type === 'ended') {
            this.events.push({ type, rc: r.u32(), completed: r.u32() });
        } else if (type === 'output') {
            const isErr = r.u8() !== 0;
            r.u8(); const text = r.str();
            this.output += (isErr ? '[stderr] ' : '') + text;
            this.events.push({ type, isErr, text });
        } else {
            this.events.push({ type });
        }
    }
    control(type, fields) {
        const w = new W.Writer();
        w.u8(1);
        w.str(W.TAG_STR, type);
        for (const f of fields || []) {
            if (typeof f === 'string') w.str(W.TAG_STR, f);
            else w.u8(f);
        }
        this.ws.send(w.toUint8Array());
    }
    submit(source, name) { this.control('submit', [name, source]); }
    ended() { return this.events.filter(e => e.type === 'ended'); }
    close() { try { this.ws.close(); } catch { /* already */ } }
}

// A client that stops reading: a raw socket that completes the WebSocket
// handshake and then pauses, so the host's writer blocks once the kernel
// buffers fill. It can still WRITE (frames are masked, as the RFC asks of a
// client), which is how it asks the host to quit.
async function stalledClient(port) {
    const sock = net.connect(port, '127.0.0.1');
    await new Promise((res, rej) => { sock.once('connect', res); sock.once('error', rej); });
    const key = crypto.randomBytes(16).toString('base64');
    sock.write('GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               + 'Sec-WebSocket-Key: ' + key + '\r\nSec-WebSocket-Version: 13\r\n\r\n');
    let head = '';
    await new Promise((res, rej) => {
        sock.on('data', function onData(d) {
            head += d.toString('latin1');
            if (head.includes('\r\n\r\n')) { sock.removeListener('data', onData); res(); }
        });
        sock.once('error', rej);
    });
    if (!/ 101 /.test(head)) throw new Error('handshake failed: ' + head.split('\r\n')[0]);
    sock.pause();                                    // from here on, nothing is read
    const send = payload => {
        const mask = crypto.randomBytes(4);
        const len = payload.length;
        const hdr = len < 126 ? [0x82, 0x80 | len] : [0x82, 0x80 | 126, len >> 8, len & 255];
        const out = Buffer.alloc(hdr.length + 4 + len);
        Buffer.from(hdr).copy(out); mask.copy(out, hdr.length);
        for (let i = 0; i < len; i++) out[hdr.length + 4 + i] = payload[i] ^ mask[i & 3];
        sock.write(out);
    };
    const control = (type, fields) => {
        const w = new W.Writer();
        w.u8(1); w.str(W.TAG_STR, type);
        for (const f of fields || []) w.str(W.TAG_STR, f);
        send(w.toUint8Array());
    };
    return { sock, control, close: () => sock.destroy() };
}

// ---------------------------------------------------------------- codec
// A tensor decoded from a reader over the wasm heap must not alias it: the
// bridge reuses the batch memory (and memory growth detaches views), while a
// store keeps the tensor for later rendering.
function codecChecks() {
    const t = new W.Tensor('float32', [3], new Float32Array([10, 20, 30]));
    const w = new W.Writer();
    w.value(t);
    const msg = w.toUint8Array();
    const heap = new Uint8Array(msg.length + 16);
    heap.set(msg, 4);                                 // payload lands 4-byte aligned
    const view = heap.subarray(4, 4 + msg.length);
    const copied = new W.Reader(view, { copyPayloads: true }).value();
    heap.fill(0);                                     // the batch is reused
    check('wasm-backed tensor payloads are copied before the heap is reused',
          copied.data[0] === 10 && copied.data[1] === 20 && copied.data[2] === 30,
          Array.from(copied.data).join(','));
}

// ---------------------------------------------------------------- host
async function main() {
    codecChecks();
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'roxal-webhost-'));
    fs.writeFileSync(path.join(root, 'helper.rox'), "func twice(x :int) -> int:\n  return x * 2\n");

    const host = spawn(roxalBin, ['--web-host', '--web-port', '0', '-p', path.join(repo, 'modules'),
                                  '--root', root, '--nocache'],
                       { cwd: repo, stdio: ['ignore', 'pipe', 'pipe'] });
    let hostOut = '', hostErr = '';
    host.stdout.on('data', d => { hostOut += d; });
    host.stderr.on('data', d => { hostErr += d; });
    const exited = new Promise(r => host.on('exit', code => r(code)));

    try {
        await waitFor(() => /listening on ws:\/\/127\.0\.0\.1:(\d+)/.test(hostOut), 20000, 'host to listen');
        const port = Number(hostOut.match(/listening on ws:\/\/127\.0\.0\.1:(\d+)/)[1]);
        check('host prints its port', port > 0, String(port));

        const c1 = new Client(`ws://127.0.0.1:${port}`);
        await c1.connect();
        await waitFor(() => c1.hello, 5000, 'hello');
        check('hello carries version and features', c1.hello.version && c1.hello.features.includes('web'),
              JSON.stringify(c1.hello));
        check('hello reports the root', c1.hello.root === fs.realpathSync(root), c1.hello.root);
        check('hello reports nothing running', c1.hello.completed === 0 && c1.hello.running === 0);

        const app = [
            'import web',
            'import helper',
            'type App object:',
            '  var count :int = 0',
            "  var msg :string = 'hi'",
            "  var t = tensor(2, 3, dtype='uint8')",
            '  proc bump(n :int):',
            '    count = count + helper.twice(n)',
            '  func shape_of(x) -> list:',
            '    return x.shape()',
            "web.expose('app', App())",
            "print('exposed')",
            'web.serve()',
            '',
        ].join('\n');
        c1.submit(app, path.join(root, 'app.rox'));

        await waitFor(() => c1.stores.generation('app') > 0, 15000, 'app store define');
        const store = c1.stores.handle('app');
        let snap = store.getSnapshot();
        check('define snapshot values', snap.count === 0 && snap.msg === 'hi', JSON.stringify(snap));
        check('tensor property arrives by value',
              snap.t instanceof W.Tensor && snap.t.dtype === 'uint8' && snap.t.shape.join('x') === '2x3'
              && snap.t.data.length === 6, String(snap.t && snap.t.shape));
        check('store lists its methods', store.methods.includes('bump') && store.methods.includes('shape_of'),
              store.methods.join(','));
        await waitFor(() => c1.output.includes('exposed'), 5000, 'print output');
        check('print reaches the client', c1.output.includes('exposed\n'), JSON.stringify(c1.output));

        const r1 = await store.call('bump', 5);
        check('store call resolves', r1 === null, String(r1));
        await waitFor(() => store.getSnapshot().count === 10, 5000, 'patch after call');
        check('patch updates the snapshot (sibling import too)', store.getSnapshot().count === 10);

        store.set('count', 100);
        await store.call('bump', 1);
        await waitFor(() => store.getSnapshot().count === 102, 5000, 'patch after write');
        check('store write is applied', store.getSnapshot().count === 102, String(store.getSnapshot().count));

        const tin = new W.Tensor('float32', [2, 2], new Float32Array([1, 2, 3, 4]));
        const shape = await store.call('shape_of', tin);
        check('tensor argument crosses to Roxal', Array.isArray(shape) && shape.join('x') === '2x2',
              JSON.stringify(shape));

        // A second client displaces the first and gets the stores again.
        const c2 = new Client(`ws://127.0.0.1:${port}`);
        await c2.connect();
        await waitFor(() => c2.hello, 5000, 'hello 2');
        check('second hello sees a running script', c2.hello.running === 1, JSON.stringify(c2.hello));
        await waitFor(() => c2.stores.generation('app') > 0, 5000, 'resync define');
        check('reconnect resends the store with current values',
              c2.stores.handle('app').getSnapshot().count === 102,
              JSON.stringify(c2.stores.handle('app').getSnapshot()));
        await waitFor(() => c1.closed, 5000, 'first client closed');
        check('first client was closed by the host', c1.closed);

        // Stop the parked script.
        c2.control('stop');
        await waitFor(() => c2.ended().length === 1, 10000, 'ended after stop');
        check('ended after stop', c2.ended()[0].rc === 0 && c2.ended()[0].completed === 1,
              JSON.stringify(c2.ended()));

        // A batch script, and a failing one.
        c2.submit("print('bye')\n", 'batch.rox');
        await waitFor(() => c2.ended().length === 2, 10000, 'batch ended');
        check('batch script ends with rc 0', c2.ended()[1].rc === 0 && c2.output.includes('bye\n'));
        c2.submit("var x = [1]\nprint(x[5])\n", 'bad.rox');
        await waitFor(() => c2.ended().length === 3, 10000, 'bad ended');
        check('failing script ends with rc 1', c2.ended()[2].rc === 1, JSON.stringify(c2.ended()[2]));
        check('error text reaches the client on stderr',
              c2.events.some(e => e.type === 'output' && e.isErr && /out-of-range/.test(e.text)),
              c2.output.slice(-200));

        // Interrupt a script that never parks.
        c2.submit("while true:\n  wait(ms=20)\n", 'spin.rox');
        await sleep(300);
        c2.control('interrupt');
        await waitFor(() => c2.ended().length === 4, 10000, 'interrupted');
        check('interrupt ends a busy script', c2.ended().length === 4);

        // Quit from a client that has stopped reading, under a feed of large
        // frames: the writer is blocked in send() on a full socket, the
        // outbound queue overflows its cap (dropped batches, resync), and
        // closing must not wait for either -- it used to hang until the
        // client resumed reading.
        const stalled = await stalledClient(port);
        await waitFor(() => c2.closed, 10000, 'the stalled client to displace c2');
        stalled.control('submit', [path.join(root, 'feed.rox'), [
            'import web',
            'type Frames object:',
            "  var img = tensor(512, 512, 3, dtype='uint8')",
            'var f = Frames()',
            "web.expose('frames', f)",
            'var i = 0',
            'while i < 300:',
            "  var t = tensor(512, 512, 3, dtype='uint8')",
            '  t[0, 0, 0] = i rem 200',
            '  f.img = t',
            '  i = i + 1',
            '  wait(ms=3)',
            'web.serve()',
            '',
        ].join('\n')]);
        await sleep(2500);
        stalled.control('quit');
        const code = await Promise.race([exited, sleep(10000).then(() => 'timeout')]);
        check('quit exits the host cleanly even with a client that stopped reading', code === 0, String(code));
        stalled.close();
        c2.close();
    } catch (e) {
        check('no exception', false, e.stack || String(e));
    } finally {
        try { host.kill('SIGKILL'); } catch { /* gone */ }
    }

    let failed = 0;
    for (const [name, ok, detail] of results) {
        console.log((ok ? 'pass' : 'FAIL') + '  ' + name + (ok || !detail ? '' : '\n      ' + detail));
        if (!ok) failed++;
    }
    if (failed) {
        console.log('\n--- host stdout ---\n' + hostOut + '\n--- host stderr ---\n' + hostErr);
    }
    console.log(`\n${results.length - failed}/${results.length} passed`);
    process.exit(failed ? 1 : 0);
}

main();
