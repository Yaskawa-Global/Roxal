import { test } from '@playwright/test';
test('bare boot probe', async ({ page }) => {
    test.setTimeout(120000);
    page.on('console', m => console.log(`[c:${m.type()}] ${m.text().slice(0,200)}`));
    page.on('pageerror', e => console.log(`[pageerror] ${e?.message || String(e)}`));
    await page.goto('/bare.html?gcthreshold=4194304&fcflags=15');
    await page.waitForTimeout(30000);
    console.log('[state] ' + JSON.stringify(await page.evaluate(() => ({
        ready: window.__bare?.ready, values: window.__bare?.values,
        crashed: window.__bare?.crashed, calls: window.__bare?.calls ?? 0, callErr: (window.__bare?.callErr ?? '').slice(0,60),
        diag: window.__bare?.mod?.ccall('roxal_diag','string',[],[]),
        stores: window.__bare?.mod?.roxalStoreNames?.(),
        log: document.getElementById('log')?.textContent?.slice(0,200),
    }))));
});
