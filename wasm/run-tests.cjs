// Run Roxal's own .rox tests through the wasm build and diff against the
// .out/.err files, so the wasm VM is held to the same expectations as native.
//
//   cd dist-mt && node ../run-tests.cjs [pattern]
//   LIMIT=100 JOBS=3 node ../run-tests.cjs
//
// One CHILD PROCESS per test, like the native runner. Doing it in-process
// instead leaks: VM::instance() is a singleton so each test needs a fresh
// module, and 200 modules at INITIAL_MEMORY plus their Worker pools will OOM
// node long before the suite finishes.

const fs = require('fs');
const path = require('path');
const { fork } = require('child_process');

// Repo root, derived from this script's own location (wasm/run-tests.cjs).
const ROXAL = process.env.ROXAL || path.resolve(__dirname, '..');
const TESTS = path.join(ROXAL, 'tests');

// ---------------------------------------------------------------- child mode
if (process.argv[2] === '--one') {
    const name = process.argv[3];
    const src = fs.readFileSync(path.join(TESTS, name + '.rox'), 'utf8');

    // stdout and stderr must stay separate: runtime-error tests keep their
    // expected stderr in a .err regex file, and .out covers stdout only.
    let out = '', err = '';
    const Module = {
        // No noInitialRun: main() must run, because it IS the VM thread. The build
        // is -sPROXY_TO_PTHREAD, so main() lands on a worker and then serves the
        // inbound script queue; calling into the VM from this thread instead would
        // run it on the host main thread, which the host refuses.
        //
        // arguments MUST be set explicitly: Emscripten otherwise forwards node's
        // own process.argv, so main() would take "--one" for a script path, fail to
        // open it, and exit before ever serving the queue.
        arguments: [],
        print: t => { out += t + '\n'; },
        printErr: t => { err += t + '\n'; },
        preRun: [function () { Module.ENV.ROXAL_GC_CONSERVATIVE = '0'; }],
    };

    (async () => {
        const createRoxal = require(path.resolve(process.cwd(), 'roxal.js'));
        const m = await createRoxal(Module);
        // Sibling .rox files that tests import must exist in the wasm FS, and
        // must go in /stdlib because that is cwd when VM::instance() resolves
        // the builtin sys/math modules.
        for (const f of fs.readdirSync(TESTS)) {
            if (f.endsWith('.rox')) {
                try { m.FS.writeFile('/stdlib/' + f, fs.readFileSync(path.join(TESTS, f))); } catch (e) {}
            }
        }
        // Name WITH .rox: diagnostics embed it and .err regexes match on it.
        // Submitting never blocks; poll for completion rather than waiting on the
        // VM thread (this thread must stay free to service its proxied FS calls).
        try {
            m.ccall('roxal_submit_source', null, ['string', 'string'], [src, name + '.rox']);
            const deadline = Date.now() + 120000;
            while (m.ccall('roxal_completed_count', 'number', [], []) < 1) {
                if (Date.now() > deadline) { err += 'TIMEOUT\n'; break; }
                await new Promise(r => setTimeout(r, 5));
            }
        } catch (e) {
            err += 'THREW: ' + (e && e.message || e) + '\n';
        }
        process.send({ out, err });
        process.exit(0);
    })().catch(e => { process.send({ out: '', err: 'LOAD FAILED: ' + e }); process.exit(0); });
    return;
}

// --------------------------------------------------------------- driver mode
const pattern = process.argv[2] || '';
const LIMIT = parseInt(process.env.LIMIT || '0', 10);
const JOBS = parseInt(process.env.JOBS || '3', 10);

function parseList(src, name) {
    const m = new RegExp(name + '\\s*=\\s*\\[([\\s\\S]*?)\\]').exec(src);
    return m ? [...m[1].matchAll(/'([^']+)'/g)].map(x => x[1]) : [];
}

// Take the test list from runtests.py rather than globbing tests/: the directory
// holds orphan .rox files that are not part of the suite (builtinmodules is one
// -- it fails natively too, with the identical parse error).
const py = fs.readFileSync(path.join(ROXAL, 'runtests.py'), 'utf8');
const known = new Set(parseList(py, 'failing_tests'));
const listed = new Set(parseList(py, 'tests').concat(parseList(py, 'test_list')));

// Mirror runtests.py's feature gating. Natively it reads tags from
// `roxal --version`; the wasm host has no CLI, so take the same information
// from the build's generated roxal_features.cmake. Without this, tests for
// features we deliberately compiled out look like wasm regressions.
// CMake puts the host in <build>/dist, so the features file is one level up from
// the working directory. Falls back to the conventional build dir name.
const BUILD = process.env.BUILD ||
    (fs.existsSync(path.join(process.cwd(), '..', 'roxal_features.cmake'))
        ? path.resolve(process.cwd(), '..')
        : path.join(ROXAL, 'build-wasm-mt'));
const featSrc = fs.readFileSync(path.join(BUILD, 'roxal_features.cmake'), 'utf8');
const has = f => new RegExp('set\\(ROXAL_HAS_' + f + ' ON\\)', 'i').test(featSrc);
const usesIcu = /ROXAL_UNICODE_BACKEND_ICU/.test(featSrc);

const gated = [];
if (!has('FILEIO')) gated.push(...parseList(py, 'fileio_tests'));
if (!has('SOCKET')) gated.push(...parseList(py, 'socket_tests'));
if (!has('FFI'))    gated.push(...parseList(py, 'ffi_tests'), ...parseList(py, 'opencv_tests'),
                               ...parseList(py, 'realsense_tests'));
if (!has('GRPC'))   gated.push(...parseList(py, 'grpc_tests'));
if (!has('DDS'))    gated.push(...parseList(py, 'dds_tests'));
if (!has('REGEX'))  gated.push(...parseList(py, 'regex_tests'));
if (!has('XML'))    gated.push(...parseList(py, 'xml_tests'));
if (!has('MEDIA'))  gated.push(...parseList(py, 'media_tests'));
if (!usesIcu)       gated.push('string_case', 'string_interp_suffix');
const skipped = new Set(gated);

const names = fs.readdirSync(TESTS)
    .filter(f => f.endsWith('.rox'))
    .map(f => f.slice(0, -4))
    .filter(n => fs.existsSync(path.join(TESTS, n + '.out')))
    .filter(n => listed.size === 0 || listed.has(n))
    .filter(n => !known.has(n))
    .filter(n => !skipped.has(n))
    .filter(n => !pattern || n.includes(pattern))
    .sort();

const selected = LIMIT > 0 ? names.slice(0, LIMIT) : names;

function runChild(name) {
    return new Promise(resolve => {
        const child = fork(__filename, ['--one', name], { cwd: process.cwd(), silent: true });
        let res = null;
        const timer = setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} }, 60000);
        child.on('message', m => { res = m; });
        child.on('exit', () => {
            clearTimeout(timer);
            resolve(res || { out: '', err: 'CHILD DIED' });
        });
    });
}

(async () => {
    let pass = 0, fail = 0, done = 0;
    const failures = [];
    const queue = selected.slice();

    async function worker() {
        while (queue.length) {
            const name = queue.shift();
            const r = await runChild(name);
            const expected = fs.readFileSync(path.join(TESTS, name + '.out'), 'utf8');
            const errFile = path.join(TESTS, name + '.err');
            let errOk = true;
            if (fs.existsSync(errFile)) {
                errOk = new RegExp(fs.readFileSync(errFile, 'utf8').trim(), 'm').test(r.err);
            }
            if (r.out === expected && errOk) pass++;
            else { fail++; failures.push({ name, got: r.out, want: expected, err: r.err, errOk }); }
            if (++done % 25 === 0) process.stderr.write(`  ...${done}/${selected.length}\n`);
        }
    }
    await Promise.all(Array.from({ length: JOBS }, worker));

    console.log(`\n${pass} passed, ${fail} failed, of ${selected.length}` +
                `  (excluded ${known.size} known-failing, ${skipped.size} feature-gated)`);
    // Every failure, not the first N. A cap here silently hid two failures behind
    // a count that said 17 -- which makes "the failures are all known ones"
    // unverifiable from the output, exactly when that claim matters most.
    failures.sort((a, b) => a.name.localeCompare(b.name));
    for (const f of failures) {
        const g = (f.got.split('\n')[0] || '(empty)').slice(0, 70);
        const w = (f.want.split('\n')[0] || '(empty)').slice(0, 70);
        console.log(`  ${f.name}${f.errOk ? '' : ' [stderr]'}\n      want: ${JSON.stringify(w)}\n      got:  ${JSON.stringify(g)}`);
    }
    process.exit(0);
})();
