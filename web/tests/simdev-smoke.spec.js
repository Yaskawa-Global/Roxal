import { test, expect } from '@playwright/test';

// Post-deploy smoke test against the live site (not the local preview): the
// ?file= parameter opens the named example, and the dataflow engine keeps
// emitting values well past where the value-stack leak used to kill it.
//
// A diagram file prints nothing at boot -- it only defines its component type.
// Output starts when Run generates the probe harness, so wait for the canvas
// to finish opening the document before clicking, or the Run lands before the
// df store holds the document and silently does nothing.
const SITE = process.env.SMOKE_URL ?? 'https://simdev.yaskawa.io';

const count = page => page.evaluate(() =>
    (document.querySelector('.output-pane')?.textContent?.match(/value = \d+/g) ?? []).length);

test('simdev honours ?file= and sustains dataflow output', async ({ page }) => {
    test.setTimeout(300000);
    const errors = [];
    page.on('pageerror', e => errors.push(String(e?.message || e)));

    await page.goto(`${SITE}/?file=counter4`);
    await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 120000 });
    await expect(page.getByRole('heading', { name: 'Counter4' })).toBeVisible({ timeout: 120000 });

    await page.getByRole('button', { name: /^Run$/ }).click();
    await expect.poll(() => count(page), { timeout: 90000 }).toBeGreaterThan(3);

    const early = await count(page);
    await page.waitForTimeout(90000);
    const later = await count(page);

    console.log(`values: ${early} -> ${later} over 90s`);
    expect(errors, 'no page errors').toEqual([]);
    expect(later, 'still emitting values').toBeGreaterThan(early + 100);
});
