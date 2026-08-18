import { test } from '@playwright/test';

// Minimal boot probe: why does the page produce nothing? Prints every console
// line, page error, failed request and the roxal.wasm response status.
test('boot probe', async ({ page }) => {
    test.setTimeout(180000);
    page.on('console', m => console.log(`[console:${m.type()}] ${m.text().slice(0, 400)}`));
    page.on('pageerror', e => console.log(`[pageerror] ${e?.message || e?.name || String(e)}`));
    page.on('requestfailed', r => console.log(`[requestfailed] ${r.url()} :: ${r.failure()?.errorText}`));
    page.on('response', r => {
        if (/roxal\.(wasm|js|data)$/.test(r.url()))
            console.log(`[response] ${r.status()} ${r.url()}`);
    });
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await page.waitForTimeout(120000);
    const state = await page.evaluate(() => ({
        hasRox: !!window.__rox,
        tabs: document.querySelectorAll('.tab').length,
        bodyText: document.body?.innerText?.slice(0, 300),
    }));
    console.log('[state] ' + JSON.stringify(state));
});
