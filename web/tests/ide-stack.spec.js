import { test } from '@playwright/test';
// Capture the SAFE_HEAP abort stack from the IDE run, with all current fixes
// and the container/node quarantine active.
test('ide crash stack', async ({ page }) => {
    test.setTimeout(180000);
    const stacks = [];
    page.on('pageerror', e => stacks.push(`PAGEERROR ${e?.message}\n${e?.stack ?? ''}`));
    page.on('console', m => { const t = m.text();
        if (/alignfault|unaligned|out of bounds|unreachable|Aborted|segmentation|wasm-function/.test(t)) stacks.push('CONSOLE ' + t); });
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/?gcthreshold=4194304&fcflags=47');
    await page.locator('.tab.active').waitFor({ timeout: 120000 });
    await page.getByRole('button', { name: /^Run$/ }).click();
    for (let i = 0; i < 60 && !stacks.length; i++) await page.waitForTimeout(1000);
    console.log('=== STACKS ===\n' + stacks.join('\n---\n').slice(0, 4000));
});
