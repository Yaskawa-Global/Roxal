import { test, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, existsSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
// AI Studio against the native host: the canvas is the window, a diagram
// opens and runs, its live values label the edges, the palette is grouped by
// module and follows the diagram's imports, the source tab shows the file,
// and the console drawer (closed by default) opens onto the output and REPL.

const repo = resolve(import.meta.dirname, '..', '..');
const roxalBin = process.env.ROXAL_BIN || join(repo, 'build', 'roxal');

test.skip(!existsSync(roxalBin), `no native roxal at ${roxalBin} (set ROXAL_BIN)`);

let host, port;
test.beforeAll(async () => {
    const root = mkdtempSync(join(tmpdir(), 'roxal-aistudio-'));
    copyFileSync(join(repo, 'web', 'src', 'demos', 'cosine.rox'), join(root, 'cosine.rox'));
    // an image-valued output and its palette-visible source (M3 views)
    for (const f of ['gen.rox', 'pic.rox', 'cam.rox'])
        copyFileSync(join(repo, 'web', 'tests', 'fixtures', f), join(root, f));
    // the model catalog alone (no weights): the palette lists a workspace's
    // models from it, with the description the catalog carries
    mkdirSync(join(root, 'models'), { recursive: true });
    copyFileSync(join(repo, 'examples', 'aistudio', 'models', 'catalog.json'),
                 join(root, 'models', 'catalog.json'));
    host = spawn(roxalBin, ['--web-host', '--web-port', '0', '-p', join(repo, 'modules'),
                            '--root', root, '--nocache'],
                 { cwd: repo, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    host.stdout.on('data', d => { out += d; });
    host.stderr.on('data', d => { process.stderr.write('[host] ' + d); });
    const deadline = Date.now() + 20000;
    while (!/listening on ws:\/\/127\.0\.0\.1:(\d+)/.test(out)) {
        if (Date.now() > deadline) throw new Error('the web host did not start:\n' + out);
        await new Promise(r => setTimeout(r, 50));
    }
    port = Number(out.match(/listening on ws:\/\/127\.0\.0\.1:(\d+)/)[1]);
});
test.afterAll(() => { try { host?.kill('SIGKILL'); } catch { /* gone */ } });

test('AI Studio opens, runs and edits a diagram on the native host', async ({ page }) => {
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    await page.goto(`/aistudio.html?host=ws://127.0.0.1:${port}&file=cosine`);

    // Boot: the only diagram in the root opens as the canvas and runs.
    await expect(page.locator('.ais-file')).toHaveText(/cosine\.rox/, { timeout: 90000 });
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 60000 });
    await expect(page.locator('.ais-status')).toContainText('native VM', { timeout: 30000 });
    await expect(page.locator('.react-flow__edge-text').first()).toContainText('0.', { timeout: 60000 });
    console.log('BOOT+RUN+LIVE ok');

    // The properties panel follows the selection: the diagram with nothing
    // selected, a node's settings when one is clicked, and it collapses.
    await expect(page.locator('.df-props h3')).toHaveText(/Cosine/);
    await page.locator('.df-node.df-func').first().click();     // math.cos
    await expect(page.locator('.df-props .df-prop').first()).toContainText('kind');
    await expect(page.locator('.df-props')).toContainText('show values on links');
    // the selected function node carries its module's docstring for it
    await expect(page.locator('.df-doc')).toContainText('cosine of x', { timeout: 20000 });
    // and a model entry carries what the workspace's catalog says about it
    await expect(page.locator('.df-section h4', { hasText: /\bmodels\b/ })).toBeVisible({ timeout: 30000 });
    await expect(page.locator('.df-palette button', { hasText: /^dfine$/ }))
        .toHaveAttribute('title', /COCO object detection/);
    console.log('NODE DOC ok');
    await page.locator('.df-props-toggle').click();
    await expect(page.locator('.df-props-body')).toHaveCount(0);
    await page.locator('.df-props-toggle').click();
    await expect(page.locator('.df-props-body')).toHaveCount(1);
    // the text output shows its value on the node while the diagram runs
    await expect(page.locator('.df-output .df-text')).toContainText(/\d/, { timeout: 30000 });
    console.log('PROPS PANEL ok');

    // The palette is grouped by module: the diagram's own import (math)
    // and the base modules.
    await expect(page.locator('.df-section h4', { hasText: /\bmath\b/ })).toBeVisible({ timeout: 30000 });
    await expect(page.locator('.df-section h4', { hasText: /\blogic\b/ })).toBeVisible();
    console.log('PALETTE SECTIONS ok');

    // "+ module…" imports another module and its functions join the palette.
    // (an in-page prompt: Electron has no window.prompt)
    await page.locator('.df-add-module').click();
    await page.locator('.ask-input').fill('opencv');
    await page.locator('.ask-input').press('Enter');
    await expect(page.locator('.df-section h4', { hasText: /\bopencv\b/ })).toBeVisible({ timeout: 30000 });
    await expect(page.locator('.df-palette button', { hasText: /^grayscale$/ })).toBeVisible();
    console.log('ADD MODULE ok');

    // Place a node from it; the source regenerates with the import.
    await page.locator('.df-palette button', { hasText: /^grayscale$/ }).click();
    await expect(page.locator('.df-node')).toHaveCount(4, { timeout: 20000 });
    await page.locator('.view-toggle button', { hasText: 'source' }).click();
    await expect.poll(async () =>
        page.evaluate(() => window.monaco?.editor.getEditors()[0]?.getModel()?.getValue() ?? ''),
        { timeout: 20000 }).toContain('opencv.grayscale');
    await page.locator('.view-toggle button', { hasText: 'diagram' }).click();
    await expect(page.locator('.df-node')).toHaveCount(4);
    console.log('SOURCE ok');

    // The console drawer is closed by default; open it, see the run's probe
    // output, and evaluate through the REPL.
    await expect(page.locator('.ais-drawer-body')).toHaveCount(0);
    await page.locator('.ais-drawer-head button').first().click();
    // (a text output shows on its node now, not in the console)
    await expect(page.locator('.ais-drawer-body .out')).toBeVisible({ timeout: 30000 });
    await page.locator('.repl-input').fill('6 * 7');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').first()).toContainText('42', { timeout: 20000 });
    console.log('DRAWER+REPL ok');

    // Stop returns the services; the status shows nothing live.
    await page.locator('.ais-stop').click();
    await expect(page.locator('.ais-status')).toContainText('○', { timeout: 30000 });
    console.log('STOP ok');

    expect(errors).toEqual([]);
});

test('an image output paints its frames at the node', async ({ page }) => {
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    await page.goto(`/aistudio.html?host=ws://127.0.0.1:${port}&file=pic`);

    await expect(page.locator('.ais-file')).toHaveText(/pic\.rox/, { timeout: 90000 });
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 60000 });
    // The output node declares view='image': it shows the selector and,
    // once the harness runs, a canvas painted from the tensor frames.
    await expect(page.locator('.df-output .df-view')).toHaveValue('image', { timeout: 30000 });
    await expect(page.locator('.df-preview canvas')).toBeVisible({ timeout: 60000 });
    // A painted frame: the fixture's gradient has a non-zero red ramp and a
    // blue channel that flips every tick, so a pixel is neither black nor
    // transparent.
    await expect.poll(async () => page.evaluate(() => {
        const c = document.querySelector('.df-preview canvas');
        if (!c || !c.width) return null;
        const d = c.getContext('2d').getImageData(20, 10, 1, 1).data;
        return [d[0], d[1], d[2], d[3]];
    }), { timeout: 30000 }).toEqual([160, 100, expect.any(Number), 255]);
    console.log('IMAGE VIEW ok');

    // Switching the view to text regenerates the source without the view
    // annotation's image value and the preview goes away.
    await page.locator('.df-output .df-view').selectOption('text');
    await expect(page.locator('.df-preview')).toHaveCount(0, { timeout: 20000 });
    await page.locator('.view-toggle button', { hasText: 'source' }).click();
    await expect.poll(async () =>
        page.evaluate(() => window.monaco?.editor.getEditors()[0]?.getModel()?.getValue() ?? ''),
        { timeout: 20000 }).toContain("view='text'");
    console.log('VIEW TOGGLE ok');

    expect(errors).toEqual([]);
});

test('a video feed drives a diagram and its rate shows at the input', async ({ page }) => {
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    await page.goto(`/aistudio.html?host=ws://127.0.0.1:${port}&file=cam`);

    await expect(page.locator('.ais-file')).toHaveText(/cam\.rox/, { timeout: 90000 });
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 60000 });
    // The input node shows its feed and, while the harness runs, the frame
    // count and rate the feed actor publishes.
    await expect(page.locator('.df-input .df-feed')).toHaveValue('video', { timeout: 30000 });
    await expect(page.locator('.df-input .df-sub')).toContainText('video:', { timeout: 10000 });
    await expect(page.locator('.df-fps')).toContainText('frames', { timeout: 60000 });
    await expect(page.locator('.df-fps')).toContainText('fps', { timeout: 30000 });
    // The grayscale output paints the (looped) test video's frames.
    await expect(page.locator('.df-preview canvas')).toBeVisible({ timeout: 60000 });
    await expect.poll(async () => page.evaluate(() => {
        const c = document.querySelector('.df-preview canvas');
        if (!c || !c.width) return null;
        const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
        let sum = 0;
        for (let i = 0; i < d.length; i += 4) sum += d[i];
        return sum > 0 ? [c.width, c.height] : null;
    }), { timeout: 30000 }).toEqual([64, 48]);
    console.log('FEED ok');

    // Clearing the feed regenerates the file without it.
    await page.locator('.df-input .df-feed').selectOption('');
    await page.locator('.view-toggle button', { hasText: 'source' }).click();
    await expect.poll(async () =>
        page.evaluate(() => window.monaco?.editor.getEditors()[0]?.getModel()?.getValue() ?? ''),
        { timeout: 20000 }).not.toContain('feed=');
    console.log('FEED CLEAR ok');

    expect(errors).toEqual([]);
});
