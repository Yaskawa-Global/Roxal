// Bridge tests for the `dom` module, run under node against a minimal DOM stub.
//
//   cd build-wasm-mt/dist && node ../../wasm/test-dom.cjs
//
// The stub is deliberately tiny: what is under test is the BRIDGE -- value
// marshalling both ways, the handle table, deferred writes and their flush
// ordering, and DOM event -> Roxal callback delivery across the thread boundary.
// Whether a real browser implements textContent is not in question.
//
// A browser check still matters for the things a stub cannot model (COOP/COEP,
// real event objects, layout); see index.html.

const path = require('path');

// ---------------------------------------------------------------- DOM stub
const listeners = [];

// Classes, not object literals. The bridge sends plain data BY VALUE and objects
// with identity BY REFERENCE, and real DOM nodes are class instances -- a stub
// built from literals would be handed over as dead copies and would not model
// the browser at all.
class StubNode {
    constructor(id) {
        this.id = id;
        this.textContent = '';
        this.style = new StubStyle();
        this.tagName = 'DIV';
        this.value = '';
    }
    addEventListener(type, handler) { listeners.push({ id: this.id, type, handler }); }
    removeEventListener(type, handler) {
        const i = listeners.findIndex(l => l.id === this.id && l.type === type && l.handler === handler);
        if (i >= 0) listeners.splice(i, 1);
    }
    getAttribute(name) { return 'attr:' + name; }
    appendChild(child) { (this.children = this.children || []).push(child); return child; }
}
class StubStyle { constructor() { this.color = ''; } }
class StubDocument {
    constructor(nodes) { this.title = 'stub page'; this._nodes = nodes; }
    getElementById(id) { return this._nodes[id] || null; }
    createElement(tag) {
        const n = new StubNode('new-' + tag);
        n.tagName = tag.toUpperCase();
        return n;
    }
}
const makeNode = id => new StubNode(id);
const nodes = { out: makeNode('out'), btn: makeNode('btn') };
const documentStub = new StubDocument(nodes);
// Deliberately NOT globalThis.window: Emscripten sniffs that to decide it is
// running in a browser, and the node loader then takes the wrong path entirely.
// The bridge looks globals up on globalThis, so this is enough.
globalThis.document = documentStub;
globalThis.someNumber = 42;
globalThis.someString = 'hi';

// Fire a stub event the way a browser would.
function dispatch(id, type, extra) {
    for (const l of listeners.filter(l => l.id === id && l.type === type))
        l.handler(Object.assign({ type, target: nodes[id] }, extra || {}));
}

// -------------------------------------------------------------------- tests
const results = [];
const check = (name, ok, detail) => results.push([name, !!ok, detail || '']);
const sleep = ms => new Promise(r => setTimeout(r, ms));

let out = '', err = '';
const Module = {
    arguments: [],       // main() serves the inbound queue rather than taking node's argv
    print:    t => { out += t + '\n'; },
    printErr: t => { err += t + '\n'; },
    onAbort:  w => { err += '[abort] ' + w + '\n'; },
};

async function run(m, src, name) {
    const before = m.ccall('roxal_completed_count', 'number', [], []);
    m.ccall('roxal_submit_source', null, ['string', 'string'], [src, name || 'test.rox']);
    const deadline = Date.now() + 60000;
    while (m.ccall('roxal_completed_count', 'number', [], []) <= before) {
        if (Date.now() > deadline) throw new Error('timed out running ' + (name || 'script'));
        await sleep(5);
    }
    return m.ccall('roxal_last_result', 'number', [], []);
}

(async () => {
    const createRoxal = require(path.resolve(process.cwd(), 'roxal.js'));
    const m = await createRoxal(Module);

    // --- module loads and populates document/window ------------------------
    out = '';
    let rc = await run(m, `
import dom
print(dom.document.title)
`, 'title.rox');
    check('dom module imports and exposes document', rc === 0 && out.includes('stub page'),
          rc === 0 ? '' : ('rc=' + rc + ' ' + err.trim().slice(0, 200)));

    // --- reads and writes across the bridge --------------------------------
    out = '';
    rc = await run(m, `
import dom
var el = dom.document.get_element_by_id("out")
el.text_content = "hello from Roxal"
print(el.text_content)
`, 'rw.rox');
    check('property write then read round-trips', rc === 0 && out.includes('hello from Roxal'),
          rc === 0 ? '' : err.trim().slice(0, 200));
    check('write actually reached the DOM', nodes.out.textContent === 'hello from Roxal',
          JSON.stringify(nodes.out.textContent));

    // --- snake_case -> camelCase, and method calls -------------------------
    out = '';
    rc = await run(m, `
import dom
var el = dom.document.get_element_by_id("out")
print(el.get_attribute("data-x"))
print(dom.get(el, "tagName"))
`, 'names.rox');
    check('snake_case method maps to camelCase', out.includes('attr:data-x'), err.trim().slice(0, 160));
    check('dom.get uses the exact name given', out.includes('DIV'));

    // --- missing element is nil, not a broken handle -----------------------
    out = '';
    rc = await run(m, `
import dom
var missing = dom.document.get_element_by_id("nope")
print(missing == nil)
`, 'nil.rox');
    check('a null JS result becomes nil', out.includes('true'), err.trim().slice(0, 160));

    // --- value marshalling -------------------------------------------------
    out = '';
    rc = await run(m, `
import dom
print(dom.global("someNumber"))
print(dom.global("someString"))
`, 'globals.rox');
    check('numbers and strings marshal from JS', out.includes('42') && out.includes('hi'),
          err.trim().slice(0, 160));

    // --- deferred writes flush before a read -------------------------------
    // The bug this guards: writes are batched, so a read that does not flush
    // first would observe a stale page.
    out = '';
    nodes.out.textContent = '';
    rc = await run(m, `
import dom
var el = dom.document.get_element_by_id("out")
el.text_content = "first"
el.text_content = "second"
print(el.text_content)
`, 'ordering.rox');
    check('batched writes flush in order before a read', out.includes('second'),
          JSON.stringify(out.trim()));

    // --- events: JS -> Roxal ------------------------------------------------
    // The script parks in dom.run(), which is how a UI app stays alive. That is
    // not incidental: handlers execute on the VM thread inside the dispatch
    // loop, so there has to be a live script for them to run under.
    out = '';
    nodes.out.textContent = '';
    const script = run(m, `
import dom
var el = dom.document.get_element_by_id("out")
dom.on(dom.document.get_element_by_id("btn"), "click", proc(e :dict):
  el.text_content = "clicked:" + e['x']
  print("handler ran, x=" + e['x'])
  dom.stop()
)
print("listening")
dom.run(5)
print("run returned")
`, 'listen.rox');

    for (let i = 0; i < 300 && !listeners.some(l => l.id === 'btn'); i++) await sleep(10);
    check('dom.on installs a listener', listeners.some(l => l.id === 'btn' && l.type === 'click'),
          err.trim().slice(0, 200));

    await sleep(50);
    dispatch('btn', 'click', { clientX: 12, clientY: 34 });
    rc = await script;

    check('DOM event reaches the Roxal handler', out.includes('handler ran, x=12'),
          JSON.stringify(out.trim().slice(0, 120)) + ' ' + err.trim().slice(0, 160));
    check('handler writes reach the DOM', nodes.out.textContent === 'clicked:12',
          JSON.stringify(nodes.out.textContent));
    check('dom.stop() ends dom.run()', out.includes('run returned'), 'rc=' + rc);

    m.ccall('roxal_quit', null, [], []);

    if (err.trim()) console.log('--- stderr ---\n' + err.trim() + '\n');
    let failed = 0;
    for (const [name, ok, detail] of results) {
        if (!ok) failed++;
        console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   (' + detail + ')' : ''}`);
    }
    console.log(failed ? `\n${failed}/${results.length} FAILED` : `\nall ${results.length} dom checks passed`);
    process.exit(failed ? 1 : 0);
})().catch(e => {
    console.error('FAILED: ' + (e && e.stack || e));
    if (err.trim()) console.error('--- stderr ---\n' + err.trim());
    process.exit(1);
});
