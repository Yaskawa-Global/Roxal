import { test, expect } from '@playwright/test';

// GC soak: the reproducer from wasm-gc-crash-repro.md, run against a
// ROXAL_GC_FORENSICS build with mark verification on (?fcflags=15).
//
// Before the GC coordination fixes this crashed in 2-8 s at a 4 MB threshold
// (`RuntimeError: memory access out of bounds`, or a SAFE_HEAP alignfault in
// free()). The pass condition is deliberately two-sided, because a frozen VM
// cannot crash and would otherwise look like a pass: the run must stay LIVE
// (output keeps arriving) AND report no forensic violation.
//
// Requires a wasm build with -DROXAL_GC_FORENSICS=1; without it the forensic
// exports are absent and the test skips rather than passing vacuously.
test('counter4 survives a GC soak with mark verification on', async ({ page }) => {
    test.setTimeout(360000);

    // Parameterised so the same harness can run bisection legs:
    //   GC_SOAK_QUERY='?fcflags=31'  (leak mode, default threshold)
    //   GC_SOAK_MS=90000 GC_SOAK_IDLE_MS=60000
    const QUERY = process.env.GC_SOAK_QUERY ?? '?gcthreshold=4194304&fcflags=15';
    const SOAK_MS = Number(process.env.GC_SOAK_MS ?? 180000);
    const IDLE_MS = Number(process.env.GC_SOAK_IDLE_MS ?? 45000);

    const crashes = [];
    page.on('pageerror', e => crashes.push(`pageerror: ${e?.message || e?.name || String(e)}${e?.stack ? '\n' + e.stack : ''}`));
    page.on('worker', w => w.on('close', () => {}));
    const log = [];
    // AddressSanitizer reports arrive as ordinary console output (many lines,
    // from a worker), not through the forensic buffer -- capture them whole,
    // and keep a generous ring so the allocation/free stacks survive.
    const asan = [];
    let inAsan = false;
    page.on('console', m => {
        const t = m.text();
        log.push(`[${m.type()}] ${t}`);
        if (log.length > 200) log.shift();
        if (/ERROR: AddressSanitizer/.test(t)) inAsan = true;
        if (inAsan) {
            asan.push(t);
            if (/SUMMARY: AddressSanitizer/.test(t)) inAsan = false;
        }
        if (/memory access out of bounds|unreachable|alignfault|unaligned|worker sent an error/i.test(t))
            crashes.push(`console: ${t}`);
    });

    await page.addInitScript(() => {
        localStorage.setItem('roxal-ide-last-file', 'counter4.rox');
    });
    await page.goto('/' + QUERY);
    // An ASan wasm is ~110MB and boots slowly; make the wait explicit rather
    // than mistaking a slow boot for a hang.
    const BOOT_MS = Number(process.env.GC_SOAK_BOOT_MS ?? 90000);
    try {
        await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: BOOT_MS });
    } catch (e) {
        console.log('--- console during boot ---\n' + log.slice(-60).join('\n'));
        if (asan.length) console.log('--- AddressSanitizer at boot ---\n' + asan.join('\n'));
        throw e;
    }

    // Provenance: never trust a browser verdict without knowing WHICH binary
    // produced it (npm run build re-runs sync-wasm without ROXAL_WASM_DIST,
    // and sync-wasm skips sources older than the destination).
    const buildInfo = await page.evaluate(() =>
        window.__rox?.ccall?.('roxal_build_info', 'string', [], []) ?? null);
    console.log('build info:', buildInfo);
    // Stale-artifact guard: a browser can serve a cached wasm (service worker,
    // disk cache, an un-refreshed tab) that looks identical to a fresh one.
    // Fail loudly rather than attribute an old binary's behaviour to new code.
    const expectBuild = process.env.GC_SOAK_EXPECT_BUILD;
    if (expectBuild && !String(buildInfo).includes(expectBuild))
        throw new Error(`STALE ARTIFACT: page reports "${buildInfo}" but expected to contain "${expectBuild}"`);
    // Content hash, not the build stamp: __TIME__ comes from ONE translation
    // unit (wasm/main.cpp), so it does not move when any other file changes --
    // a freshly built binary reported an unchanged stamp during this work.
    // Hash what the page actually fetched instead.
    const expectSha = process.env.GC_SOAK_EXPECT_SHA;
    if (expectSha) {
        const got = await page.evaluate(async () => {
            const buf = await (await fetch('/roxal.wasm')).arrayBuffer();
            const d = await crypto.subtle.digest('SHA-256', buf);
            return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 16);
        });
        console.log('served wasm sha:', got);
        if (got !== expectSha)
            throw new Error(`STALE ARTIFACT: served wasm ${got} != expected ${expectSha}`);
    }

    const hasForensics = await page.evaluate(() => {
        try { window.__rox.ccall('roxal_forensic_count', 'number', [], []); return true; }
        catch { return false; }
    });
    test.skip(!hasForensics, 'wasm build lacks ROXAL_GC_FORENSICS');

    await page.getByRole('button', { name: /^Run$/ }).click();

    const outputLines = () => page.evaluate(() =>
        document.querySelector('.output')?.textContent?.split('\n').length ?? 0);

    const start = Date.now();
    let lastCount = await outputLines();
    let lastTicks = 0;
    let lastProgress = Date.now();

    while (Date.now() - start < SOAK_MS) {
        await page.waitForTimeout(5000);

        if (asan.length && !inAsan) {
            console.log('--- AddressSanitizer report ---\n' + asan.join('\n'));
            throw new Error(`AddressSanitizer report after ${Math.round((Date.now() - start) / 1000)}s (see above)`);
        }
        const violations = await page.evaluate(() =>
            window.__rox.ccall('roxal_forensic_count', 'number', [], []));
        if (violations > 0) {
            const report = await page.evaluate(() =>
                window.__rox.ccall('roxal_forensic_report', 'string', [], []));
            throw new Error(`GC forensic violation after ${Math.round((Date.now() - start) / 1000)}s: ${report}`);
        }
        if (crashes.length) {
            await page.waitForTimeout(2000);   // let the worker's own message land
            let report = '(unavailable)';
            try {
                report = await page.evaluate(() => window.__rox.ccall('roxal_forensic_report', 'string', [], []));
            } catch { /* worker may be gone */ }
            if (asan.length)
                console.log('--- AddressSanitizer report ---\n' + asan.join('\n'));
            console.log('--- console tail ---\n' + log.join('\n'));
            console.log('--- forensic report ---\n' + report);
            throw new Error(`crash after ${Math.round((Date.now() - start) / 1000)}s: ${crashes.join(' | ')}`);
        }

        const count = await outputLines();
        if (count !== lastCount) { lastCount = count; lastProgress = Date.now(); }
        // Output text is a POOR liveness proxy: a diagram file renders its
        // values on the canvas, so a perfectly healthy VM can print nothing.
        // The VM's own counters are the real signal.
        const diag = await page.evaluate(() =>
            { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return ''; } });
        const ticks = Number(/engineTicks=(\d+)/.exec(diag)?.[1] ?? 0)
                    + Number(/pump=(\d+)/.exec(diag)?.[1] ?? 0);
        if (ticks !== lastTicks) { lastTicks = ticks; lastProgress = Date.now(); }
        // Liveness half of the verdict: a wedged VM stops allocating and can
        // never crash, so silence must fail rather than pass.
        expect(Date.now() - lastProgress,
            `VM produced no output for ${IDLE_MS / 1000}s (frozen, not merely crash-free)`).toBeLessThan(IDLE_MS);
    }

    expect(crashes, 'no wasm faults during the soak').toEqual([]);
});
