import { test } from '@playwright/test';
// The IDE opens AND RUNS the last-used file at startup, which is why it crashes
// with no Run pressed. Does the crash need the counter4 dataflow program, or
// does IDE startup alone do it? Same page, different last file.
const file = process.env.IDE_FILE ?? 'counter4.rox';
test(`ide startup file A/B`, async ({ page }) => {
    test.setTimeout(240000);
    let crashed = null;
    page.on('pageerror', e => { crashed = crashed || (e?.message || e?.name || String(e)); });
    const conlog = [];
    page.on('console', m => { const t = m.text();
        conlog.push(`[${m.type()}] ${t.slice(0,300)}`); if (conlog.length > 40) conlog.shift();
        if (/out of bounds|unaligned|unreachable|worker sent an error|assertion|hardening|abort/i.test(t))
            crashed = crashed || t.slice(0,300); });
    await page.addInitScript(f => localStorage.setItem('roxal-ide-last-file', f), file);
    await page.goto('/' + (process.env.IDE_QUERY ?? '?gcthreshold=4194304&fcflags=15'));
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    for (let i = 0; i < 30 && !crashed; i++) await page.waitForTimeout(3000);
    const diag = await page.evaluate(() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return 'gone'; } });
    const hist = await page.evaluate(() => { try { return window.__rox.ccall('roxal_drain_histogram','string',[],[]); } catch { return 'gone'; } });
    console.log(`RESULT hist=${hist}`);
    const out = await page.evaluate(() => document.querySelector('.output')?.textContent?.trim().slice(-120) ?? '');
    console.log(`RESULT file=${file} crashed=${crashed} output="${out.replace(/\n/g,' | ')}"`);
    console.log(`RESULT diag=${diag}`);
    console.log('RESULT console tail:\n' + conlog.slice(-12).join('\n'));
});
