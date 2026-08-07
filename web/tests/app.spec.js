import { test, expect } from '@playwright/test';

// Each check below corresponds to something that actually broke, or that only a
// browser can tell us. Nothing here duplicates the node suites.

test.beforeEach(async ({ page }) => {
    // Surface page errors in the test output. A dead inline script or a trapped
    // VM worker otherwise shows up only as "the UI never updated".
    page.on('pageerror', e => console.error('[pageerror]', e.message));
    page.on('console', m => { if (m.type() === 'error') console.error('[console]', m.text()); });
    await page.goto('/');
});

test('the document is cross-origin isolated', async ({ page }) => {
    // Without this SharedArrayBuffer is unavailable and the VM cannot spawn the
    // GC reclaimer or the dataflow engine thread -- it cannot be constructed at
    // all. A missing header on whatever serves the app breaks everything, so it
    // is worth asserting directly rather than inferring from a later failure.
    expect(await page.evaluate(() => self.crossOriginIsolated)).toBe(true);
});

test('the VM boots and Roxal publishes its state to React', async ({ page }) => {
    // Scope to the panel. The editor renders the Roxal source, so a bare text
    // selector matches the CODE as well as the UI -- which is exactly how this
    // test started failing when Monaco was added.
    const panel = page.locator('.panel');
    await expect(panel.getByText('temperature', { exact: true })).toBeVisible();
    await expect(page.locator('.v.big')).toHaveText('20.0°');
    await expect(panel.getByText('idle', { exact: true }).first()).toBeVisible();
});

test('a React click runs a Roxal method and the network converges', async ({ page }) => {
    await expect(page.locator('.v.big')).toHaveText('20.0°');

    await page.locator('.panel').getByRole('button', { name: 'preheat 150' }).click();
    await expect(page.locator('.log')).toHaveText('preheat');

    // The signal network converges the temperature and the `when heating becomes
    // false` event fires. Whole circuit: React -> method -> signal -> dataflow
    // -> store -> re-render.
    await expect(page.locator('.v.big')).toHaveText('150.0°', { timeout: 20_000 });
    await expect(page.locator('.log')).toContainText('settled at');
});

test('the network animates rather than jumping', async ({ page }) => {
    // Guards the signal path specifically. If signal changes stopped reaching the
    // store (as they did before M3), a single final value would still appear via
    // some other update and the test above would pass. Intermediate values prove
    // the signal is publishing continuously.
    await page.locator('.panel').getByRole('button', { name: 'preheat 150' }).click();

    const seen = new Set();
    const deadline = Date.now() + 12_000;
    while (Date.now() < deadline) {
        const v = await page.locator('.v.big').textContent();
        seen.add(v);
        if (v === '150.0°') break;
        await page.waitForTimeout(50);
    }
    expect(seen.size, `only saw ${[...seen].join(', ')}`).toBeGreaterThan(3);
});

test('a React write goes INTO the signal, not over it', async ({ page }) => {
    // store.set on a signal property must push the value into the signal. Writing
    // over the property would replace the signal object and silently demolish
    // every derived node feeding off it -- the value would still read fine.
    const slider = page.locator('.panel input[type=range]');
    await slider.fill('100');
    await expect(page.locator('.v.big')).toHaveText('100.0°', { timeout: 20_000 });

    // The derived nodes must still be alive afterwards.
    await slider.fill('60');
    await expect(page.locator('.v.big')).toHaveText('60.0°', { timeout: 20_000 });
});

test('the VM survives a full move and stays responsive', async ({ page }) => {
    // A trapped VM worker is silent -- the page just stops updating. Running two
    // moves in sequence catches a VM that died during the first.
    await page.locator('.panel').getByRole('button', { name: 'reflow 240' }).click();
    await expect(page.locator('.v.big')).toHaveText('240.0°', { timeout: 25_000 });

    await page.locator('.panel').getByRole('button', { name: 'cool' }).click();
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 25_000 });
});

test('the Monaco editor mounts and holds the Roxal source', async ({ page }) => {
    // No language is registered (no highlighting, no IntelliSense) but the editor
    // must be there and populated. Assert on the MODEL, not the DOM: Monaco
    // virtualises and only renders the visible lines.
    await expect(page.locator('.monaco-editor')).toBeVisible();
    const text = await page.evaluate(() => window.monaco.editor.getModels()[0].getValue());
    expect(text).toContain('type Oven object');
    expect(text).toContain('web.expose("oven", oven)');
});

test('an edited script can be re-run against the live VM', async ({ page }) => {
    await expect(page.locator('.v.big')).toHaveText('0.00');

    // Edit through the model, then Run. The parked app must stop, the new script
    // must start, and re-exposing "arm" must REPLACE the old object -- otherwise
    // the edit is silently ignored, the worst failure mode a live editor has.
    // Change the LOGIC, not a string: a string could be overwritten by a startup
    // event and prove nothing. If preheat now targets 111, the edited script is
    // genuinely the one running.
    await page.evaluate(() => {
        const m = window.monaco.editor.getModels()[0];
        m.setValue(m.getValue().replace('setpoint.set(150.0)', 'setpoint.set(111.0)'));
    });
    await expect(page.locator('.dirty')).toBeVisible();

    await page.getByRole('button', { name: 'Run' }).click();

    // Still live after the re-run, and running the EDITED network.
    await page.locator('.panel').getByRole('button', { name: 'preheat 150' }).click();
    await expect(page.locator('.v.big')).toHaveText('111.0°', { timeout: 25_000 });
});
