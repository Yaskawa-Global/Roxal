import { test } from '@playwright/test';
// Does the IDE crash WITHOUT a script transition? The app replaces/destroys
// stores on every script change (bootstrap -> app -> Run), which the bare page
// never does. If it crashes with no Run at all, transitions are not required.
test('ide idle, never pressing Run', async ({ page }) => {
    test.setTimeout(300000);
    let crashed = null;
    page.on('pageerror', e => { crashed = crashed || (e?.message || e?.name || String(e)); });
    page.on('console', m => { const t = m.text();
        if (/out of bounds|unaligned|unreachable|worker sent an error/i.test(t)) crashed = crashed || t; });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    for (let i = 0; i < 24 && !crashed; i++) await page.waitForTimeout(5000);
    const diag = await page.evaluate(() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return 'gone'; } });
    console.log(`[idle result] crashed=${crashed} diag=${diag}`);
});
