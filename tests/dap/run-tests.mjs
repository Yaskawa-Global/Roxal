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
