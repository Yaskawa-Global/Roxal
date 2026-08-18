import { test, expect } from '@playwright/test';

// Discriminator: does the 7s crash need the DIAGRAM EDITOR's sampling loop, or
// does the running program crash on its own?
//
// The soak opens counter4.rox, which the IDE opens as a React Flow canvas whose
// live-value poll calls the df store ~2.5x/s (DfEditor.jsx:258). That store
// path -- actor call, inspect mirror build, JS marshalling -- is the browser-only
// machinery the headless reproducer cannot reach. Here the SAME program runs
// with the editor showing SOURCE instead, so no sampling occurs.
//
// Crash in both  => the fault is in the running program / GC, not the editor path.
// Crash only here-not => it is the sampling/store path, and that is where to look.
test('counter4 with the source view (no diagram sampling)', async ({ page }) => {
    test.setTimeout(300000);
    const SOAK_MS = Number(process.env.GC_SOAK_MS ?? 120000);

    const crashes = [];
    page.on('pageerror', e => crashes.push(`pageerror: ${e?.message || e?.name || String(e)}`));
    page.on('console', m => {
        const t = m.text();
        if (/memory access out of bounds|unreachable|alignfault|unaligned|worker sent an error/i.test(t))
            crashes.push(`console: ${t}`);
    });

    await page.addInitScript(() => {
        localStorage.setItem('roxal-ide-last-file', 'counter4.rox');
    });
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 90000 });

    // Switch the pane to Source so the React Flow canvas -- and with it the
    // sampling interval -- is not mounted.
    const srcToggle = page.getByRole('button', { name: /^Source$/ });
    if (await srcToggle.count()) await srcToggle.click();
    await page.waitForTimeout(500);

    await page.getByRole('button', { name: /^Run$/ }).click();

    const start = Date.now();
    let lastCount = 0, lastProgress = Date.now();
    while (Date.now() - start < SOAK_MS) {
        await page.waitForTimeout(5000);
        if (crashes.length)
            throw new Error(`crash after ${Math.round((Date.now() - start) / 1000)}s: ${crashes[0]}`);
        const n = await page.evaluate(() =>
            document.querySelector('.output')?.textContent?.split('\n').length ?? 0);
        if (n !== lastCount) { lastCount = n; lastProgress = Date.now(); }
        expect(Date.now() - lastProgress, 'VM still producing output').toBeLessThan(60000);
    }
    console.log(`survived ${SOAK_MS / 1000}s with ${lastCount} output lines and no sampling`);
});
