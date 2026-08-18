import { test } from '@playwright/test';
// Reproduce David's report: value output stops (at 11) in the IDE. Count value
// updates and correlate the stall with GC counters.
test('ide value stall', async ({ page }) => {
    test.setTimeout(1500000);
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/' + (process.env.IDE_QUERY ?? ''));
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    await page.getByRole('button', { name: /^Run$/ }).click();
    const rows = [];
    let last = -1, stalledAt = null, lastVal = '';
    for (let i = 0; i < Number(process.env.STALL_ITERS ?? 60); i++) {
        await page.waitForTimeout(3000);
        const st = await page.evaluate(() => {
            const t = document.querySelector('.output-pane')?.textContent ?? '';
            const m = t.match(/value = \d+/g) ?? [];
            return { n: m.length, last: m[m.length - 1] ?? '',
                diag: (() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return 'gone'; } })() };
        });
        rows.push(`t=${(i+1)*3}s n=${st.n} last="${st.last}" ${st.diag}`);
        if (st.n === last && stalledAt === null && i > 1) { stalledAt = (i+1)*3; lastVal = st.last; }
        last = st.n;
    }
    console.log(`STALLED_AT=${stalledAt} lastValue="${lastVal}" totalUpdates=${last}`);
    console.log(rows.filter((_, i) => i < 2 || (stalledAt && (i+1)*3 >= stalledAt - 6 && (i+1)*3 <= stalledAt + 9) || i === 59).join('\n'));
});
