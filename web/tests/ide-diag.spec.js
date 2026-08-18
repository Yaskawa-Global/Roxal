import { test } from '@playwright/test';
// What is the IDE's GC state doing in the seconds before the 7s crash, versus
// the bare page which survives? Poll roxal_diag until the crash.
test('ide diag before crash', async ({ page }) => {
    test.setTimeout(120000);
    const rows = [];
    let crashed = null;
    page.on('pageerror', e => { crashed = crashed || (e?.message || String(e)); });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await page.locator('.tab.active').waitFor({ timeout: 90000 });
    await page.getByRole('button', { name: /^Run$/ }).click();
    for (let i = 0; i < 40 && !crashed; i++) {
        await page.waitForTimeout(500);
        const d = await page.evaluate(() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return null; } });
        if (d) rows.push(`t=${(i+1)*0.5}s ${d}`);
    }
    console.log('[crashed] ' + crashed);
    console.log(rows.slice(-6).join('\n'));
});
