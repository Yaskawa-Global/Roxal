import { test, expect } from '@playwright/test';

// The second example: two cascading tanks. What is worth testing here is not
// the pixels but that a SECOND app can be opened, run and driven -- the panel
// follows whichever store the running script exposed.
test('the tanks example runs, fills, overflows and drains', async ({ page }) => {
    // This test is about the 2D panel; the default view is 3D now.
    await page.addInitScript(() => localStorage.setItem('roxal-ide-tanks-view', '2d'));
    // tanks.rox is what a fresh visitor gets, and it runs on boot.
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/tanks\.rox/, { timeout: 90000 });
    await expect(page.locator('.tank')).toHaveCount(2, { timeout: 60000 });

    // Torricelli: tank 1 settles where its drain rate matches the tap. Assert
    // that it SETTLES rather than climbing -- an algebraic loop in the network
    // (drain(level) instead of drain(level[-1])) freezes the drain node and
    // fills the tank forever, which is exactly what this caught in draft.
    const level1 = () => page.locator('.tank-unit').first()
        .locator('.v.big').textContent().then(t => parseFloat(t));
    await page.getByRole('button', { name: 'steady' }).click();
    await expect.poll(level1, { timeout: 20000 }).toBeGreaterThan(2.5);
    await page.waitForTimeout(2000);
    expect(await level1()).toBeLessThan(4.5);

    // Open the tap past what tank 1 can drain and it overflows.
    await page.getByRole('button', { name: 'surge' }).click();
    await expect(page.locator('.tank.spilling')).toHaveCount(1, { timeout: 25000 });
    await expect(page.locator('.log.alarm')).toContainText('overflowing');

    // Shut it and both tanks empty again.
    await page.getByRole('button', { name: 'shut' }).click();
    await expect.poll(level1, { timeout: 30000 }).toBeLessThan(0.2);
    await expect(page.locator('.tank.spilling')).toHaveCount(0);
});

test('the source pane collapses, keeping its toolbar', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.monaco-editor')).toBeVisible({ timeout: 90000 });

    await page.getByRole('button', { name: /source/ }).click();
    await expect(page.locator('.monaco-editor')).toBeHidden();
    // The File menu, the tabs and Run have to survive -- hiding them with the
    // editor would strand the user with no way back.
    await expect(page.locator('.menu summary')).toBeVisible();
    await expect(page.locator('.tab.active')).toBeVisible();
    await expect(page.getByRole('button', { name: /^Run$/ })).toBeVisible();

    // ...and the preference outlives a reload.
    await page.reload();
    await expect(page.locator('.tab.active')).toBeVisible({ timeout: 90000 });
    await expect(page.locator('.monaco-editor')).toBeHidden();

    await page.getByRole('button', { name: /source/ }).click();
    await expect(page.locator('.monaco-editor')).toBeVisible();
});

// The Babylon view: same tanks.rox, same store, a WebGL scene instead of DOM.
// The assertions read window.__tanks3d -- the values the SCENE last applied --
// so a pass means the whole chain held: signals -> store -> direct
// subscription -> per-frame render. Headless Chromium renders via SwiftShader.
test('the 3D view renders the same store', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/tanks\.rox/, { timeout: 90000 });
    await expect(page.locator('.tank, .tanks3d-canvas').first()).toBeVisible({ timeout: 60000 });

    // Ensure 3D (the default, but a stored 2D preference must not fail this).
    await page.getByRole('button', { name: '3D' }).click();
    await expect(page.locator('.tanks3d-canvas')).toBeVisible({ timeout: 30000 });
    await expect.poll(() => page.evaluate(() => window.__tanks3d?.ready ?? false),
                      { timeout: 30000 }).toBe(true);

    // Drive the plant through the SHARED controls and watch the scene follow.
    await page.getByRole('button', { name: 'surge' }).click();
    await expect.poll(() => page.evaluate(() => window.__tanks3d.level1),
                      { timeout: 30000 }).toBeGreaterThan(9);
    await page.getByRole('button', { name: 'shut' }).click();
    await expect.poll(() => page.evaluate(() => window.__tanks3d.level1),
                      { timeout: 30000 }).toBeLessThan(1);

    // Both views stay in lockstep because there is only one source of truth.
    await page.getByRole('button', { name: '2D' }).click();
    await expect(page.locator('.tank')).toHaveCount(2, { timeout: 20000 });
});

// Pause stops the RENDERER while the app keeps running: the scene's numbers
// freeze, the network's do not -- and resuming snaps to wherever the plant
// actually got to in the meantime.
test('pause stops rendering, not the app', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.tanks3d-canvas')).toBeVisible({ timeout: 90000 });
    await expect.poll(() => page.evaluate(() => window.__tanks3d?.ready ?? false),
                      { timeout: 30000 }).toBe(true);

    // Set the plant FILLING so "the level is changing" is unambiguous.
    await page.getByRole('button', { name: 'surge' }).click();
    await expect.poll(() => page.evaluate(() => window.__tanks3d.level1),
                      { timeout: 20000 }).toBeGreaterThan(1);

    await page.getByRole('button', { name: '⏸' }).click();
    const frozen = await page.evaluate(() => window.__tanks3d.level1);
    await page.waitForTimeout(2000);
    // The scene stopped applying updates...
    expect(await page.evaluate(() => window.__tanks3d.level1)).toBe(frozen);

    await page.getByRole('button', { name: '▶' }).click();
    // ...but the NETWORK kept integrating: on resume the scene catches up to a
    // level well past where the renderer froze.
    await expect.poll(() => page.evaluate(() => window.__tanks3d.level1),
                      { timeout: 20000 }).toBeGreaterThan(frozen + 0.5);
});
