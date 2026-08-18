import { test } from '@playwright/test';
// The crash is fixed; a freeze is now visible (it was masked by the earlier
// 7s crash). Poll output length and roxal_diag to localise where it wedges.
test('freeze diagnosis', async ({ page }) => {
    test.setTimeout(300000);
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    const rows = [];
    let last = -1, stalledAt = null;
    for (let i = 0; i < 40; i++) {
        await page.waitForTimeout(3000);
        const st = await page.evaluate(() => ({
            out: document.querySelector('.output-pane')?.textContent?.length ?? 0,
            diag: (() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return 'gone'; } })(),
        }));
        rows.push(`t=${(i+1)*3}s out=${st.out} ${st.diag}`);
        if (st.out === last && stalledAt === null && i > 2) stalledAt = (i+1)*3;
        last = st.out;
    }
    console.log(`STALLED_AT=${stalledAt}`);
    console.log(rows.filter((_, i) => i % 4 === 0 || i > 30).join('\n'));
});
