import { test } from '@playwright/test';
// Is "df sample: unwind" a CAUSE or a SYMPTOM? Log the timestamp of the first
// unwind warning and of the first crash. If unwind only appears after the
// wasm trap, it is the dead worker rejecting later calls -- a symptom.
test('unwind vs crash ordering', async ({ page }) => {
    test.setTimeout(240000);
    const t0 = Date.now();
    const events = [];
    page.on('pageerror', e => events.push(`${((Date.now()-t0)/1000).toFixed(1)}s CRASH ${e?.message}\nSTACK:\n${(e?.stack ?? '').slice(0, 2500)}`));
    page.on('console', m => { const t = m.text();
        if (/unwind/i.test(t)) events.push(`${((Date.now()-t0)/1000).toFixed(1)}s UNWIND ${t.slice(0,60)}`);
        else events.push(`${((Date.now()-t0)/1000).toFixed(1)}s [${m.type()}] ${t.slice(0,900)}`); });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/' + (process.env.IDE_QUERY ?? '?gcthreshold=4194304&fcflags=15'));
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    for (let i = 0; i < 40; i++) await page.waitForTimeout(1000);
    const st = await page.evaluate(() => ({
        tab: document.querySelector('.tab.active')?.textContent,
        diag: (() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch (e) { return 'gone:' + e; } })(),
        out: document.querySelector('.output-pane')?.textContent?.trim().slice(-600),
    }));
    console.log(`=== VALIDITY tab=${st.tab} out="${st.out}" diag=${st.diag}`);
    console.log('=== ORDER ===\n' + (events.slice(0, 14).join('\n') || '(no events)'));
});
