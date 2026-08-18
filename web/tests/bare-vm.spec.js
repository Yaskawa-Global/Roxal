import { test, expect } from '@playwright/test';

// Runs the SAME counter4 harness the IDE runs, on a bare page with no React,
// no IDE stores and no editor (public/bare.html). Headless under node that
// harness survives 90s; in the IDE it faults at ~7s. This splits the
// difference: a crash here means the browser wasm environment itself, not the
// IDE's store/DOM traffic.
test('bare page: counter4 harness with no IDE', async ({ page }) => {
    test.setTimeout(300000);
    const SOAK_MS = Number(process.env.BARE_SOAK_MS ?? 120000);

    const crashes = [];
    page.on('pageerror', e => crashes.push(`pageerror: ${e?.message || e?.name || String(e)}`));
    page.on('console', m => {
        const t = m.text();
        if (/memory access out of bounds|unreachable|alignfault|unaligned|worker sent an error/i.test(t))
            crashes.push(`console: ${t}`);
    });

    const QUERY = process.env.BARE_QUERY ?? '?gcthreshold=4194304&fcflags=15';
    await page.goto('/bare.html' + QUERY);
    await expect.poll(() => page.evaluate(() => window.__bare?.ready === true),
                      { timeout: 90000 }).toBe(true);

    const start = Date.now();
    let lastValues = 0, lastProgress = Date.now();
    while (Date.now() - start < SOAK_MS) {
        await page.waitForTimeout(3000);
        const st = await page.evaluate(() => ({
            values: window.__bare?.values ?? 0,
            crashed: window.__bare?.crashed ?? null,
            forensic: window.__bare?.forensic ?? null,
            count: window.__bare?.count ?? 0,
        }));
        const secs = Math.round((Date.now() - start) / 1000);
        if (st.count > 0) throw new Error(`forensic violation at ${secs}s: ${st.forensic}`);
        if (crashes.length || st.crashed)
            throw new Error(`crash at ${secs}s: ${crashes[0] ?? st.crashed} | forensic: ${st.forensic}`);
        if (st.values !== lastValues) { lastValues = st.values; lastProgress = Date.now(); }
        // A run with no collections proves nothing about a GC bug.
        const diag = await page.evaluate(() => window.__bare?.mod?.ccall('roxal_diag','string',[],[]) ?? '');
        const cols = Number(/collections=(\d+)/.exec(diag)?.[1] ?? 0);
        if (Date.now() - start > 30000 && cols === 0)
            throw new Error(`INVALID LEG: no collections after 30s (${diag})`);
        expect(Date.now() - lastProgress, 'VM still emitting values').toBeLessThan(45000);
    }
    const hist = await page.evaluate(() => { try { return window.__bare.mod.ccall('roxal_drain_histogram','string',[],[]); } catch { return 'gone'; } });
    console.log(`bare page survived ${SOAK_MS / 1000}s with ${lastValues} values`);
    console.log(`RESULT hist=${hist}`);
});
