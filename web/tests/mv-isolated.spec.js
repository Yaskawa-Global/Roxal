import { test } from '@playwright/test';
// The heap-validating build (emmalloc-memvalidate) on its OWN server (:4174),
// so other experiments redeploying :4173 cannot invalidate it. It validates
// the whole heap on every malloc/free, so it aborts at the FIRST operation
// that sees corruption rather than at the eventual victim. Very slow to boot.
test('memvalidate: ide startup', async ({ page }) => {
    test.setTimeout(2400000);
    const hits = [];
    page.on('pageerror', e => hits.push(`PAGEERROR ${e?.message}\n${(e?.stack ?? '').slice(0, 3000)}`));
    page.on('console', m => { const t = m.text();
        if (/Aborted|assert|corrupt|alignfault|unaligned|out of bounds|unreachable|memvalidate/i.test(t))
            hits.push('CONSOLE ' + t.slice(0, 1500)); });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('http://localhost:4174/?gcthreshold=4194304&fcflags=15');
    for (let i = 0; i < 1200 && !hits.length; i++) {
        await page.waitForTimeout(2000);
        if (i % 30 === 0) {
            const st = await page.evaluate(() => ({ tabs: document.querySelectorAll('.tab').length,
                diag: (() => { try { return window.__rox.ccall('roxal_diag','string',[],[]); } catch { return null; } })() }));
            console.log(`t=${i*2}s tabs=${st.tabs} ${st.diag ?? ''}`);
        }
    }
    console.log('=== HITS ===\n' + hits.join('\n---\n').slice(0, 6000));
});
