import { test } from '@playwright/test';
// emmalloc-memvalidate checks the whole heap on every malloc/free, so it aborts
// AT the operation that first sees corrupt metadata -- much closer to the
// culprit than the eventual malloc/free that trips over it. Very slow.
test('ide under heap validation', async ({ page }) => {
    test.setTimeout(1800000);
    const hits = [];
    page.on('pageerror', e => hits.push(`PAGEERROR ${e?.message}\n${(e?.stack ?? '').slice(0, 2500)}`));
    page.on('console', m => { const t = m.text();
        if (/Aborted|assert|corrupt|alignfault|unaligned|out of bounds|unreachable/i.test(t))
            hits.push('CONSOLE ' + t.slice(0, 1200)); });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/?gcthreshold=4194304&fcflags=15');
    await page.locator('.tab.active').waitFor({ timeout: 900000 });
    console.log('booted');
    await page.getByRole('button', { name: /^Run$/ }).click();
    for (let i = 0; i < 600 && !hits.length; i++) await page.waitForTimeout(1000);
    console.log('=== HITS ===\n' + hits.join('\n---\n').slice(0, 5000));
});
