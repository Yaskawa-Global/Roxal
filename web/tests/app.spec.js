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
    // Assert on the MODEL, not the DOM: Monaco virtualises and only renders the
    // visible lines, so a DOM query would miss most of the file.
    await expect(page.locator('.monaco-editor')).toBeVisible();
    const text = await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().getValue());
    expect(text).toContain('type Oven object');
    expect(text).toContain('web.expose("oven", oven)');
});

test('an edited script can be re-run against the live VM', async ({ page }) => {
    await expect(page.locator('.v.big')).toHaveText('20.0°');

    // Edit through the model, then Run. The parked app must stop, the new script
    // must start, and re-exposing "oven" must REPLACE the old object -- otherwise
    // the edit is silently ignored, the worst failure mode a live editor has.
    // Change the LOGIC, not a string: a string could be overwritten by a startup
    // event and prove nothing. If preheat now targets 111, the edited script is
    // genuinely the one running.
    await page.evaluate(() => {
        const m = window.monaco.editor.getEditors()[0].getModel();
        m.setValue(m.getValue().replace('setpoint.set(150.0)', 'setpoint.set(111.0)'));
    });
    await expect(page.locator('.tab.active')).toContainText('•');

    await page.getByRole('button', { name: 'Run' }).click();
    // Wait for the re-run to FINISH. Clicking into the app while the restart is
    // still in flight drives the old network, which the restart then discards --
    // the assertion below would fail for a reason that has nothing to do with
    // whether the edit took effect.
    await expect(page.getByRole('button', { name: /Run|running/ }))
        .toHaveText('Run', { timeout: 30_000 });

    // Still live after the re-run, and running the EDITED network.
    await page.locator('.panel').getByRole('button', { name: 'preheat 150' }).click();
    await expect(page.locator('.v.big')).toHaveText('111.0°', { timeout: 25_000 });
});

// --- language service ------------------------------------------------------
// These exercise the whole chain: Monaco -> store method call -> VM thread ->
// inspect.parse -> back. A Monarch tokenizer could not produce any of it, so a
// passing test here really does mean the compiler front end is answering.

const markers = page => page.evaluate(
    () => window.monaco.editor.getModelMarkers({ owner: 'roxal' })
        .map(m => ({ message: m.message, line: m.startLineNumber, col: m.startColumn })));

test('diagnostics come from the real compiler front end', async ({ page }) => {
    await expect(page.locator('.monaco-editor')).toBeVisible();
    await expect.poll(() => markers(page).then(m => m.length)).toBe(0);

    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel()
        .setValue('var x =\nproc oops(:\n'));

    await expect.poll(() => markers(page).then(m => m.length), { timeout: 20_000 })
        .toBeGreaterThan(0);
    const found = await markers(page);

    // ANTLR's phrasing, i.e. the parser really ran -- not a regex guess.
    expect(found.some(m => /no viable alternative|extraneous|mismatched/.test(m.message))).toBe(true);
    // The "expecting {...}" tail is trimmed for the gutter.
    expect(found.every(m => !m.message.includes('expecting'))).toBe(true);
    // Roxal columns are 0-based and Monaco's are 1-based; a 0 here means the
    // conversion was dropped, which silently shifts every squiggle.
    expect(found.every(m => m.col >= 1)).toBe(true);

    // And they clear again -- a service that only ever adds markers is useless.
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue('var x :int = 1\n'));
    await expect.poll(() => markers(page).then(m => m.length), { timeout: 20_000 }).toBe(0);
});

// Showing a hover is a one-shot: if the VM has not booted yet the provider
// returns null and no widget appears, and nothing re-triggers it. So re-trigger
// on every poll rather than triggering once and waiting on a locator.
// Triggered once per call and then waited on patiently: describe() parses the
// document and walks every node on the VM thread, which takes appreciably longer
// than a hover normally does, and re-triggering mid-flight just restarts it.
const hoverAt = (page, line, column) => page.evaluate(async ({ line, column }) => {
    const ed = window.monaco.editor.getEditors()[0];
    ed.focus();
    // Always arrive from somewhere else: Monaco will not re-query the provider
    // for a position it has already answered for, so a retry that stays put
    // silently returns the first answer forever.
    // A visible hover is NOT re-rendered for a new position -- showHover is a
    // no-op while one is up, so without this the second lookup silently returns
    // the first one's answer.
    ed.trigger('test', 'editor.action.hideHover', {});
    ed.setPosition({ lineNumber: line, column });

    const shown = () => Array.from(document.querySelectorAll('.monaco-hover-content'))
        .map(e => e.textContent ?? '').join(' ').trim();
    ed.trigger('test', 'editor.action.showHover', {});
    for (let i = 0; i < 50; i++) {
        const text = shown();
        if (text) return text;
        await new Promise(r => setTimeout(r, 100));
    }
    return '';
}, { line, column });

test('hover reports the type the compiler deduced', async ({ page }) => {
    await expect(page.locator('.monaco-editor')).toBeVisible();
    // The panel only shows a value once the script has run, and that same script
    // exposes the service -- so this is the "the service exists" signal.
    await expect(page.locator('.v.big')).toHaveText('20.0°');

    // `total` is real and `count` is int, and NEITHER is written down: both
    // types are deduced. That is the whole point of going through the front end.
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel()
        .setValue('var count = 41\nvar total = count * 2.5\n'));

    await expect.poll(() => hoverAt(page, 2, 14), { timeout: 30_000 })   // in `count`
        .toContain('int');
    await expect.poll(() => hoverAt(page, 2, 6), { timeout: 30_000 })    // in `total`
        .toContain('real');
});
