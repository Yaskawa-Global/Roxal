import { test, expect } from '@playwright/test';

// Debugger in the Monaco demo: toggle debugging, set a gutter breakpoint,
// run — the program stops on the line, the panel shows the stack and
// locals, stepping and continue work, and after the session the IDE is
// still alive.
//
// The spec drives breakpoints through window.__roxalDebug (the same policy
// as window.monaco: tests drive the model, not pixel coordinates in the
// gutter).
test('breakpoint, inspect, step, continue in the IDE', async ({ page }) => {
    test.setTimeout(240000);
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/oven\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });

    // A plain serving script with an obvious loop to break in.
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue(
        ['import web',
         '',
         'type App object:',
         '  var beat :int = 0',
         '',
         'var app = App()',
         'web.expose("app", app)',
         '',
         'proc tick():',
         '  var x = app.beat + 1',
         '  app.beat = x',
         '',
         'var t = 0',
         'for i in range(..<80):',
         '  tick()',
         '  t = t + 1',
         '  wait(ms=25)',
         'print(t)',
         ''].join('\n')));

    // Debug on, breakpoint on `var x = app.beat + 1` (line 10).
    await page.locator('.debug-toggle').click();
    await expect(page.locator('.debug-panel')).toBeVisible();
    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(10));

    await page.getByRole('button', { name: /^Run$/ }).click();

    // The stop arrives and the panel describes it.
    await expect(page.locator('.debug-status')).toContainText('stopped (breakpoint)',
                                                              { timeout: 60000 });
    await expect(page.locator('.debug-status')).toContainText(':10');
    await expect(page.locator('.debug-stack li').first()).toContainText('tick');
    // Locals of the stopped frame -- x is not yet live on the first hit, but
    // the frame is inspectable; step once and x appears.
    await page.getByRole('button', { name: /over/ }).click();
    await expect(page.locator('.debug-status')).toContainText('stopped (step)',
                                                              { timeout: 30000 });
    await expect(page.locator('.debug-vars')).toContainText('x', { timeout: 20000 });

    // Clear the breakpoint (toggle off), continue: the loop (~2s left) runs
    // freely and ends DETACHED; the debug poll notices the dead session and
    // restores the IDE services -- the panel says so explicitly.
    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(10));
    await page.getByRole('button', { name: /continue/ }).click();
    await expect(page.locator('.debug-status')).toContainText('no debuggable program',
                                                              { timeout: 60000 });

    // Debug off; the restored console answers.
    await page.locator('.debug-toggle').click();
    await expect(page.locator('.debug-panel')).toHaveCount(0);
    await expect(async () => {
        await page.locator('.repl-input').fill('6 * 7');
        await page.locator('.repl-input').press('Enter');
        await expect(page.locator('.repl-log')).toContainText('42', { timeout: 4000 });
    }).toPass({ timeout: 30000, intervals: [3000] });
});

// The flow that matters in practice: the page boots and a LONG-RUNNING
// batch program owns the VM (with wait() fixed, a second-ticking loop runs
// for minutes).  Only then is debugging toggled.  The toggle must displace
// the running program (grace, then interrupt), restart it under the session,
// arm the gutter breakpoint live, and stop -- and afterwards a signal-driven
// app must still run (the interrupt stopped the dataflow engine; the host
// restarts it).
test('toggle debugging while a batch program is running', async ({ page }) => {
    test.setTimeout(240000);
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/oven\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });

    // A second-ticking batch loop, started NORMALLY (not debugging).
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue(
        ['import web',
         'var t = 0',
         'for i in range(..<100):',
         '  t = t + 1',
         '  wait(1s)',
         "  print('tick:' + string(t))",
         "print('done')",
         ''].join('\n')));
    await page.getByRole('button', { name: /^Run$/ }).click();
    // Wait for OUR script's second tick -- a bare '2' matched stale pane
    // content, firing the toggle 65ms after Run (a good race repro, a bad
    // test).
    await expect(page.locator('.pane-body.out')).toContainText('tick:2', { timeout: 30000 });

    // NOW toggle debugging: the running loop is displaced and restarted
    // under the session, with no error note.
    await page.locator('.debug-toggle').click();
    await expect(page.locator('.debug-status')).toContainText('running — gutter click',
                                                              { timeout: 60000 });
    await expect(page.locator('.run-error')).toHaveCount(0);

    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(4));
    await expect(page.locator('.debug-status')).toContainText('stopped (breakpoint)',
                                                              { timeout: 30000 });
    await expect(page.locator('.debug-status')).toContainText(':4');
    await expect(page.locator('.debug-vars')).toContainText('t', { timeout: 20000 });

    // The restart ran as a DEBUG run (not a workspace-keyed run with a
    // stale flag): past runScript's 20s deadline there is still no error.
    await page.waitForTimeout(21000);
    await expect(page.locator('.run-error')).toHaveCount(0);

    // Continue hits the breakpoint again on the next iteration (1s away):
    // the panel passes through running and stops afresh.
    await page.getByRole('button', { name: /continue/ }).click();
    await expect(page.locator('.debug-status')).toContainText('running',
                                                              { timeout: 30000 });
    await expect(page.locator('.debug-status')).toContainText('stopped (breakpoint)',
                                                              { timeout: 30000 });

    // Clear the bp, continue, debug off (releases everything).
    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(4));
    await page.getByRole('button', { name: /continue/ }).click();
    await expect(page.locator('.debug-status')).toContainText('running',
                                                              { timeout: 30000 });
    await page.locator('.debug-toggle').click();
    await expect(page.locator('.debug-panel')).toHaveCount(0);

    // The dataflow engine survived the interrupt: a signal app still works.
    page.on('dialog', d => d.accept());
    await page.locator('.menu summary').click();
    await page.getByRole('button', { name: 'oven.rox' }).click();
    await expect(page.locator('.v.big')).toHaveText(/°/, { timeout: 60000 });
});

// Debugging a program that was ALREADY RUNNING when the page loaded -- the
// flow you get by simply reloading the page.  Three separate defects lived
// here, and each fails these tests differently:
//
//   * the gutter click armed a breakpoint against the program still being
//     displaced -- which had no debugger to release it, so it froze for good;
//   * the pre-run push replayed a stale breakpoint set, erasing the click
//     (set_breakpoints REPLACES the set for a source), so the restarted
//     program ran straight past the line;
//   * the restart opened with a store call nothing serviced, so it cost the
//     bridge's full 20s timeout instead of ~2s.
//
// WHEN the gutter is clicked decides which of those it hits, so both orders
// are covered: during the restart, and after the session is live.
async function bootIntoRunningProgram(page) {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/oven\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });

    // Make the active file a counting batch program and save it by running it,
    // so a reload boots straight into a program that owns the VM.
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue(
        ['import web',
         'var t = 0',
         'for i in range(..<400):',
         '  t = t + 1',
         "  print('tick:' + string(t))",
         '  wait(ms=500)',
         ''].join('\n')));
    await page.getByRole('button', { name: /^Run$/ }).click();
    await expect(page.locator('.pane-body.out')).toContainText('tick:2', { timeout: 30000 });

    await page.reload();
    await expect(page.locator('.pane-body.out')).toContainText('tick:3', { timeout: 90000 });
}

// Release everything so the next spec does not inherit a frozen program.
async function releaseDebugSession(page, line) {
    await page.evaluate(l => window.__roxalDebug.toggleBreakpoint(l), line);
    await page.getByRole('button', { name: /continue/ }).click();
    await page.locator('.debug-toggle').click();
    await expect(page.locator('.debug-panel')).toHaveCount(0);
}

test('debug a running program, breakpoint clicked DURING the restart', async ({ page }) => {
    test.setTimeout(240000);
    await bootIntoRunningProgram(page);

    await page.locator('.debug-toggle').click();
    await page.waitForTimeout(1000);            // the restart is still in flight
    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(4));

    // The session comes up promptly -- 15s is far outside the ~2s this takes
    // and far inside the 21s the unserviced store call used to cost.
    await expect(page.locator('.debug-status')).toContainText('running — gutter click',
                                                             { timeout: 15000 });
    // ...and the breakpoint clicked mid-restart is the one that fires.
    await expect(page.locator('.debug-status')).toContainText('stopped (breakpoint)',
                                                             { timeout: 30000 });
    await expect(page.locator('.debug-status')).toContainText(':4');
    await expect(page.getByRole('button', { name: /continue/ })).toBeVisible();
    await expect(page.locator('.debug-vars')).toContainText('t', { timeout: 20000 });
    await expect(page.locator('.run-error')).toHaveCount(0);

    await releaseDebugSession(page, 4);
});

test('debug a running program, breakpoint clicked AFTER the session is live',
     async ({ page }) => {
    test.setTimeout(240000);
    await bootIntoRunningProgram(page);

    await page.locator('.debug-toggle').click();
    // Wait for the restart to finish before touching the gutter: the live-edit
    // path through the running session, not the pre-run barrier.
    await expect(page.locator('.debug-status')).toContainText('running — gutter click',
                                                             { timeout: 15000 });
    await expect(page.locator('.run-error')).toHaveCount(0);

    await page.evaluate(() => window.__roxalDebug.toggleBreakpoint(4));
    await expect(page.locator('.debug-status')).toContainText('stopped (breakpoint)',
                                                             { timeout: 30000 });
    await expect(page.locator('.debug-status')).toContainText(':4');
    await expect(page.getByRole('button', { name: /continue/ })).toBeVisible();
    await expect(page.locator('.debug-vars')).toContainText('t', { timeout: 20000 });

    await releaseDebugSession(page, 4);
});
