#!/usr/bin/env node
// DAP transcript harness on the wasm/run-tests.cjs pattern: spawn
// `roxal --dap`, speak the protocol over stdio, assert the event/response
// sequences.  No dependencies.
//
//   node tests/dap/run-tests.mjs            # uses $ROXAL, else build-rel, else build
//   ROXAL=build/roxal node tests/dap/run-tests.mjs

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');
const roxal = process.env.ROXAL
  ? path.resolve(process.env.ROXAL)
  : ['build-rel/roxal', 'build/roxal']
      .map(p => path.join(repoRoot, p))
      .find(existsSync);
if (!roxal) {
  console.error('roxal binary not found (set ROXAL=)');
  process.exit(2);
}

const fixture = name => path.join(here, name);

class DapClient {
  constructor(args = []) {
    this.proc = spawn(roxal, ['--dap', ...args], { cwd: repoRoot });
    this.buf = Buffer.alloc(0);
    this.seq = 1;
    this.pending = new Map();   // request_seq -> resolve
    this.events = [];           // received, unconsumed
    this.eventWaiters = [];     // {name, pred, resolve, timer}
    this.stderr = '';
    this.exited = new Promise(res => this.proc.on('exit', code => res(code)));
    this.proc.stdout.on('data', d => this.onData(d));
    this.proc.stderr.on('data', d => { this.stderr += d.toString(); });
  }

  onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    for (;;) {
      const hdrEnd = this.buf.indexOf('\r\n\r\n');
      if (hdrEnd < 0) return;
      const header = this.buf.slice(0, hdrEnd).toString();
      const m = /content-length:\s*(\d+)/i.exec(header);
      if (!m) { console.error('bad header:', header); process.exit(2); }
      const len = parseInt(m[1], 10);
      const bodyStart = hdrEnd + 4;
      if (this.buf.length < bodyStart + len) return;
      const body = this.buf.slice(bodyStart, bodyStart + len).toString();
      this.buf = this.buf.slice(bodyStart + len);
      this.onMessage(JSON.parse(body));
    }
  }

  onMessage(msg) {
    if (msg.type === 'response') {
      const r = this.pending.get(msg.request_seq);
      if (r) { this.pending.delete(msg.request_seq); r(msg); }
      return;
    }
    if (msg.type === 'event') {
      for (let i = 0; i < this.eventWaiters.length; ++i) {
        const w = this.eventWaiters[i];
        if (msg.event === w.name && w.pred(msg)) {
          this.eventWaiters.splice(i, 1);
          clearTimeout(w.timer);
          w.resolve(msg);
          return;
        }
      }
      this.events.push(msg);
    }
  }

  send(command, args) {
    const seq = this.seq++;
    const body = JSON.stringify({ type: 'request', seq, command, arguments: args ?? {} });
    this.proc.stdin.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
    return new Promise(res => this.pending.set(seq, res));
  }

  waitEvent(name, pred = () => true, timeoutMs = 15000) {
    for (let i = 0; i < this.events.length; ++i) {
      const e = this.events[i];
      if (e.event === name && pred(e)) {
        this.events.splice(i, 1);
        return Promise.resolve(e);
      }
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error(`timeout waiting for event '${name}'`));
      }, timeoutMs);
      this.eventWaiters.push({ name, pred, resolve, timer });
    });
  }

  outputText() {
    return this.events
      .filter(e => e.event === 'output')
      .map(e => e.body?.output ?? '')
      .join('');
  }
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });
const assert = (cond, why) => { if (!cond) throw new Error(why); };

// ---------------------------------------------------------------------------
test('breakpoint_step_variables', async () => {
  const c = new DapClient();
  const init = await c.send('initialize', { adapterID: 'roxal', columnsStartAt1: true });
  assert(init.success, 'initialize failed');
  const launchP = c.send('launch', { program: fixture('fixture_basic.rox') });
  await c.waitEvent('initialized');
  const launch = await launchP;
  assert(launch.success, `launch failed: ${launch.message}`);

  const bp = await c.send('setBreakpoints', {
    source: { path: fixture('fixture_basic.rox') },
    breakpoints: [{ line: 4 }],
  });
  assert(bp.success && bp.body.breakpoints.length === 1, 'setBreakpoints shape');
  assert(bp.body.breakpoints[0].verified === true, 'breakpoint not verified after launch');
  assert(bp.body.breakpoints[0].line === 4, 'breakpoint line adjusted unexpectedly');

  await c.send('configurationDone');
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint');
  const tid = stopped.body.threadId;

  const threads = await c.send('threads');
  assert(threads.body.threads.some(t => t.id === tid), 'stopped thread not listed');

  const st = await c.send('stackTrace', { threadId: tid });
  const frames = st.body.stackFrames;
  assert(frames.length >= 2, 'expected work + module frames');
  assert(frames[0].name === 'work', `top frame ${frames[0].name}`);
  assert(frames[0].line === 4, `top frame line ${frames[0].line}`);
  assert(frames[0].source.path.endsWith('fixture_basic.rox'), 'frame source path');

  const scopes = await c.send('scopes', { frameId: frames[0].id });
  const locals = scopes.body.scopes.find(s => s.name === 'Locals');
  assert(locals, 'Locals scope missing');
  const vars = await c.send('variables', { variablesReference: locals.variablesReference });
  const byName = Object.fromEntries(vars.body.variables.map(v => [v.name, v]));
  assert(byName.n && byName.n.value === '10', `n = ${byName.n?.value}`);
  assert(byName.total, 'total missing from locals');

  // Tier 1 hover/watch evaluation
  const ev = await c.send('evaluate', {
    expression: 'n', frameId: frames[0].id, context: 'hover' });
  assert(ev.success && ev.body.result === '10', `evaluate n = ${ev.body?.result}`);
  const bad = await c.send('evaluate', {
    expression: 'nosuch.thing', frameId: frames[0].id, context: 'hover' });
  assert(!bad.success, 'bogus path should fail cleanly');

  // Live edit FIRST: clear the loop-line breakpoint (it would preempt the
  // step on every iteration -- breakpoints outrank steps by design), then
  // step and expect a pure step stop.
  await c.send('setBreakpoints', {
    source: { path: fixture('fixture_basic.rox') }, breakpoints: [],
  });
  const step = await c.send('next', { threadId: tid });
  assert(step.success, 'next failed');
  const stepStop = await c.waitEvent('stopped', e => e.body.reason === 'step');
  assert(stepStop.body.threadId === tid, 'step stopped on wrong thread');

  const cont = await c.send('continue', { threadId: tid });
  assert(cont.success, 'continue failed');
  await c.waitEvent('continued');
  await c.waitEvent('exited', e => e.body.exitCode === 0);
  await c.waitEvent('terminated');
  await c.send('disconnect');
  await c.exited;
  assert(c.outputText().includes('result=45'), 'program output missing from output events');
});

// ---------------------------------------------------------------------------
test('stop_on_entry_and_locations', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', {
    program: fixture('fixture_basic.rox'), stopOnEntry: true,
  });
  await c.waitEvent('initialized');
  await launchP;

  const locs = await c.send('breakpointLocations', {
    source: { path: fixture('fixture_basic.rox') }, line: 1, endLine: 8,
  });
  assert(locs.success, 'breakpointLocations failed');
  const lines = locs.body.breakpoints.map(b => b.line);
  assert(lines.includes(4) && lines.includes(7), `locations ${JSON.stringify(lines)}`);

  await c.send('configurationDone');
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'entry');
  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  // The module top-level frame carries the module's name.
  assert(st.body.stackFrames[0].name === 'fixture_basic', 'entry stop not at module frame');
  assert(st.body.stackFrames[0].line === 1, `entry stop at line ${st.body.stackFrames[0].line}`);
  await c.send('continue', { threadId: stopped.body.threadId });
  await c.waitEvent('exited');
  await c.send('disconnect');
  await c.exited;
});

// ---------------------------------------------------------------------------
test('pause_inspect_terminate', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_spin.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('configurationDone');

  await new Promise(r => setTimeout(r, 300));   // let the loop spin
  const pause = await c.send('pause', { threadId: 1 });
  assert(pause.success, `pause failed: ${pause.message}`);
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'pause');

  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  assert(st.body.stackFrames.length >= 1, 'no frames while paused');
  const scopes = await c.send('scopes', { frameId: st.body.stackFrames[0].id });
  assert(scopes.body.scopes.length >= 1, 'no scopes while paused');

  await c.send('terminate');
  await c.waitEvent('exited');
  await c.waitEvent('terminated');
  await c.send('disconnect');
  const code = await c.exited;
  assert(code === 0, `exit code ${code}`);
});

// ---------------------------------------------------------------------------
test('exception_stop_and_evaluate', async () => {
  const c = new DapClient();
  const init = await c.send('initialize', { adapterID: 'roxal' });
  assert(init.body.exceptionBreakpointFilters?.some(f => f.filter === 'error'),
         'error filter not advertised');
  const launchP = c.send('launch', { program: fixture('fixture_error.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('setExceptionBreakpoints', { filters: ['error'] });
  await c.send('configurationDone');

  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'exception');
  assert((stopped.body.text ?? '').includes('out-of-range'), 'stop text missing error');
  const tid = stopped.body.threadId;

  const st = await c.send('stackTrace', { threadId: tid });
  const frames = st.body.stackFrames;
  assert(frames[0].name === 'mid' && frames[0].line === 3,
         `exception frame ${frames[0].name}:${frames[0].line}`);

  const xi = await c.send('exceptionInfo', { threadId: tid });
  assert(xi.success && xi.body.description.includes('out-of-range'),
         'exceptionInfo missing description');

  // The failed frame is fully inspectable, hover included.
  const ev = await c.send('evaluate', {
    expression: 'lst[2]', frameId: frames[0].id, context: 'hover' });
  assert(ev.success && ev.body.result === '3', `lst[2] = ${ev.body?.result}`);
  const evx = await c.send('evaluate', {
    expression: 'x', frameId: frames[0].id, context: 'watch' });
  assert(evx.success && evx.body.result === '10', `x = ${evx.body?.result}`);

  // Continue: the pre-existing teardown then runs exactly once.
  await c.send('continue', { threadId: tid });
  await c.waitEvent('exited', e => e.body.exitCode === 1);
  await c.waitEvent('terminated');
  await c.send('disconnect');
  await c.exited;
  assert(c.outputText().includes('out-of-range'), 'error diagnostic not in output events');
});

// ---------------------------------------------------------------------------
test('uncaught_exception_stop', async () => {
  // A THROWN exception with no handler anywhere (vs a direct fatal error)
  // must stop at the raise site with frames intact -- the provably-uncaught
  // pre-unwind check.
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_throw.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('setExceptionBreakpoints', { filters: ['error'] });
  await c.send('configurationDone');

  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'exception');
  assert((stopped.body.text ?? '').includes('Divide by 0'), 'stop text missing exception');
  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  const frames = st.body.stackFrames;
  assert(frames[0].name === 'div' && frames[0].line === 2,
         `uncaught stop frame ${frames[0].name}:${frames[0].line}`);
  const ev = await c.send('evaluate', {
    expression: 'b', frameId: frames[0].id, context: 'hover' });
  assert(ev.success && ev.body.result === '0', `b = ${ev.body?.result}`);

  await c.send('continue', { threadId: stopped.body.threadId });
  await c.waitEvent('exited', e => e.body.exitCode === 1);
  await c.send('disconnect');
  await c.exited;
});

// ---------------------------------------------------------------------------
test('client_eof_failsafe', async () => {
  // A client that vanishes without a polite disconnect (crash, kill -9)
  // must not orphan the debuggee: adapter EOF fail-safes into terminate.
  const c = new DapClient();
  await c.send('initialize', {});
  const launchP = c.send('launch', { program: fixture('fixture_spin.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('configurationDone');
  await new Promise(r => setTimeout(r, 300));   // let the loop spin
  c.proc.stdin.destroy();                        // abrupt client death
  const code = await Promise.race([
    c.exited,
    new Promise((_, rej) => setTimeout(
      () => rej(new Error('adapter did not exit after client EOF')), 20000)),
  ]);
  assert(code === 0, `exit code ${code}`);
});

// ---------------------------------------------------------------------------
// A breakpoint while an actor sits in a native call longer than the old 3 s
// stop budget: the stop stays pending and lands when the call returns --
// never skipped.  The actor's thread is named by its type.
test('pending_stop_over_native_call', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_blocked.rox') });
  await c.waitEvent('initialized');
  await launchP;
  const bp = await c.send('setBreakpoints', {
    source: { path: fixture('fixture_blocked.rox') }, breakpoints: [{ line: 9 }] });
  assert(bp.body.breakpoints[0].verified, 'breakpoint not verified');
  assert(Number.isInteger(bp.body.breakpoints[0].id), 'breakpoint has no id');
  const t0 = Date.now();
  await c.send('configurationDone');
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint', 10000);
  const waited = Date.now() - t0;
  assert(waited > 3000, `stopped after ${waited} ms: the actor's call had not returned`);
  assert(!c.outputText().includes('stop failed'), 'the stop failed: ' + c.outputText());
  assert(!c.outputText().includes('past the breakpoint line'), 'the breakpoint line ran');
  const th = await c.send('threads');
  const names = th.body.threads.map(t => t.name);
  assert(names.includes('main'), `threads ${names}`);
  assert(names.some(n => /^Blocker #\d+$/.test(n)), `no actor-named thread in ${names}`);
  await c.send('continue', { threadId: stopped.body.threadId });
  await c.waitEvent('exited', e => e.body.exitCode === 0);
  await c.send('disconnect');
  await c.exited;
  assert(c.outputText().includes('past the breakpoint line'), 'program did not finish');
});

// ---------------------------------------------------------------------------
// Pause answers at once; the stop lands when the actor's call returns.
test('pause_pending_answers_at_once', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_blocked_loop.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('configurationDone');
  await new Promise(r => setTimeout(r, 300));
  const t0 = Date.now();
  const p = await c.send('pause', { threadId: 1 });
  const answered = Date.now() - t0;
  assert(p.success, `pause failed: ${p.message}`);
  assert(answered < 500, `pause answered after ${answered} ms`);
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'pause', 8000);
  assert(Date.now() - t0 > 1500, 'stopped before the actor returned');
  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  assert(st.body.stackFrames.length >= 1, 'no frames');
  await c.send('terminate');
  await c.waitEvent('exited');
  await c.send('disconnect');
  await c.exited;
});

// ---------------------------------------------------------------------------
// Vectors, matrices and orients show their numbers, and expand.
test('values_show_numbers', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_values.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('setBreakpoints', { source: { path: fixture('fixture_values.rox') }, breakpoints: [{ line: 4 }] });
  await c.send('configurationDone');
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint');
  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  const scopes = await c.send('scopes', { frameId: st.body.stackFrames[0].id });
  const mod = scopes.body.scopes.find(s => s.name === 'Module');
  assert(mod, 'no Module scope');
  const vars = await c.send('variables', { variablesReference: mod.variablesReference });
  const by = Object.fromEntries(vars.body.variables.map(v => [v.name, v]));
  assert(by.v?.value === '[1 2.5 3]', `v = ${by.v?.value}`);
  assert(by.m?.value === '[[1 2] [3 4]]', `m = ${by.m?.value}`);
  assert(/y=90/.test(by.o?.value ?? ''), `o = ${by.o?.value}`);
  const vk = await c.send('variables', { variablesReference: by.v.variablesReference });
  assert(vk.body.variables.map(x => `${x.name}=${x.value}`).join(' ') === '[0]=1 [1]=2.5 [2]=3',
         `v children ${JSON.stringify(vk.body.variables)}`);
  const ok = await c.send('variables', { variablesReference: by.o.variablesReference });
  const on = Object.fromEntries(ok.body.variables.map(x => [x.name, x.value]));
  assert(on.y === '90°' && on.r === '0°' && on.quaternion, `o children ${JSON.stringify(on)}`);
  const ev = await c.send('evaluate', { expression: 'v', frameId: st.body.stackFrames[0].id, context: 'hover' });
  assert(ev.body?.result === '[1 2.5 3]', `evaluate v = ${ev.body?.result}`);
  await c.send('continue', { threadId: stopped.body.threadId });
  await c.waitEvent('exited');
  await c.send('disconnect');
  await c.exited;
});

// ---------------------------------------------------------------------------
// A breakpoint set before its module compiles is reported unverified (with
// an id), then verified by a `breakpoint` changed event; the importer's
// Module scope lists its own names and an expensive Imported scope the
// rest; `source` serves a module's text.
test('imports_breakpoints_scopes_source', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const early = await c.send('setBreakpoints', {
    source: { path: fixture('fixture_lib.rox') }, breakpoints: [{ line: 3 }] });
  const b0 = early.body.breakpoints[0];
  assert(b0.verified === false && Number.isInteger(b0.id), `early ${JSON.stringify(b0)}`);
  const launchP = c.send('launch', { program: fixture('fixture_importer.rox') });
  await c.waitEvent('initialized');
  await launchP;
  const changed = await c.waitEvent('breakpoint', e => e.body.breakpoint.id === b0.id, 5000);
  assert(changed.body.reason === 'changed' && changed.body.breakpoint.verified === true
         && changed.body.breakpoint.line === 3, `changed ${JSON.stringify(changed.body)}`);
  await c.send('setBreakpoints', { source: { path: fixture('fixture_importer.rox') }, breakpoints: [{ line: 3 }] });
  await c.send('configurationDone');
  const s1 = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint');
  const st1 = await c.send('stackTrace', { threadId: s1.body.threadId });
  assert(st1.body.stackFrames[0].name === 'helper', `top ${st1.body.stackFrames[0].name}`);
  assert(st1.body.stackFrames[0].source.path.endsWith('fixture_lib.rox'), 'lib frame path');
  const src = await c.send('source', { source: { path: fixture('fixture_lib.rox') } });
  assert(src.success && src.body.content.includes('func helper(x):'), `source ${src.message}`);
  await c.send('continue', { threadId: s1.body.threadId });
  const s2 = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint');
  const st2 = await c.send('stackTrace', { threadId: s2.body.threadId });
  const scopes = await c.send('scopes', { frameId: st2.body.stackFrames[0].id });
  const mod = scopes.body.scopes.find(s => s.name === 'Module');
  const imp = scopes.body.scopes.find(s => s.name === 'Imported');
  assert(mod && imp && imp.expensive === true && mod.expensive === false,
         `scopes ${JSON.stringify(scopes.body.scopes)}`);
  const own = (await c.send('variables', { variablesReference: mod.variablesReference })).body.variables.map(v => v.name);
  const imported = (await c.send('variables', { variablesReference: imp.variablesReference })).body.variables.map(v => v.name);
  assert(own.includes('mine') && !own.includes('helper') && !own.includes('libvar'), `own ${own}`);
  assert(imported.includes('helper') && imported.includes('libvar') && !imported.includes('mine'),
         `imported ${imported}`);
  await c.send('continue', { threadId: s2.body.threadId });
  await c.waitEvent('exited');
  await c.send('disconnect');
  await c.exited;
});

// ---------------------------------------------------------------------------
// `attach`: answered at once with `initialized`; a launch after it refused.
test('attach_then_launch_refused', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const a = await c.send('attach', {});
  assert(a.success, `attach failed: ${a.message}`);
  await c.waitEvent('initialized', () => true, 3000);
  const l = await c.send('launch', { program: fixture('fixture_basic.rox') });
  assert(!l.success, 'launch after attach should be refused');
  await c.send('disconnect');
  const code = await Promise.race([c.exited,
    new Promise((_, rej) => setTimeout(() => rej(new Error('adapter did not exit')), 10000))]);
  assert(code === 0, `exit code ${code}`);
});

// ---------------------------------------------------------------------------
// Tier 2 (read-only): watch / repl evaluate expressions and calls in the
// stopped frame; hover stays a path; an error is the evaluation's, not the
// program's; a call waiting on a parked actor is refused; the run goes on.
test('evaluate_expressions_in_frame', async () => {
  const c = new DapClient();
  await c.send('initialize', { adapterID: 'roxal' });
  const launchP = c.send('launch', { program: fixture('fixture_eval.rox') });
  await c.waitEvent('initialized');
  await launchP;
  await c.send('setBreakpoints', { source: { path: fixture('fixture_eval.rox') }, breakpoints: [{ line: 15 }] });
  await c.send('configurationDone');
  const stopped = await c.waitEvent('stopped', e => e.body.reason === 'breakpoint');
  const st = await c.send('stackTrace', { threadId: stopped.body.threadId });
  const frameId = st.body.stackFrames[0].id;
  const ev = (expression, context = 'repl') => c.send('evaluate', { expression, context, frameId });
  const expect = async (expr, want, context) => {
    const r = await ev(expr, context);
    assert(r.success && r.body.result === want, `${expr} -> ${r.body?.result ?? r.message} (wanted ${want})`);
  };
  await expect('total + 1', '16');
  await expect('helper(total)', '30');
  await expect('len([1, 2, 3])', '3');
  await expect('[1.0 2.0] * 2', '[2 4]', 'watch');
  await expect('n', '5', 'watch');
  const hover = await ev('total + 1', 'hover');
  assert(!hover.success, 'hover evaluated an expression');
  const div = await ev('total / 0');
  assert(!div.success && /zero|divi/i.test(div.message), `division: ${div.message}`);
  const bad = await ev('nosuch + 1');
  assert(!bad.success, 'an unknown name evaluated');
  const blocked = await ev('b.get() + 1');
  assert(!blocked.success && /waits|stopped|did not finish/i.test(blocked.message), `actor call: ${blocked.message}`);
  // A statement-action value typed at the console is reported, not run; as a
  // watch it is just a value.
  const stmt = await ev('cmd()');
  assert(!stmt.success && /statement/.test(stmt.message), `cmd() as a statement: ${stmt.message ?? stmt.body?.result}`);
  const asWatch = await ev('cmd()', 'watch');
  assert(asWatch.success && /^Cmd\{/.test(asWatch.body.result), `cmd() as a watch: ${asWatch.body?.result ?? asWatch.message}`);
  const again = await ev('total');
  assert(again.success && again.body.result === '15', 'the frame changed');
  await c.send('continue', { threadId: stopped.body.threadId });
  await c.waitEvent('exited', e => e.body.exitCode === 0);
  await c.send('disconnect');
  await c.exited;
  assert(c.outputText().includes('r=15'), 'the program did not finish normally');
  assert(!c.outputText().includes('ran'), 'a statement action ran from an evaluation');
});

// ---------------------------------------------------------------------------
let failed = 0;
for (const t of tests) {
  try {
    await t.fn();
    console.log(`Test: ${t.name} passed`);
  } catch (e) {
    failed++;
    console.log(`Test: ${t.name} FAILED - ${e.message}`);
  }
}
console.log(`DAP tests: Passed ${tests.length - failed} failed ${failed}`);
process.exit(failed ? 1 : 0);
