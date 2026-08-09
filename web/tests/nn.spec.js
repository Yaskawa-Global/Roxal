import { test, expect } from '@playwright/test';

// ai.nn in the browser: mnist.rox loads /models/mnist-8.onnx from the wasm
// preload and exposes a "digit" store; the panel draws into a 28x28 grid and
// hands it to Roxal, which runs the model through onnxruntime-web.
//
// The node suite proves numerical parity with native. This proves the browser
// wiring end to end -- provider registration, /ort/ asset serving under COEP,
// replies arriving through the inbound drain, list arguments crossing into
// Roxal and the prediction coming back out -- and, by asserting the digit,
// that the 784 pixels land in the tensor the right way round.
//
// Headless CI has no WebGPU, so this exercises the CPU path; the WebGPU path
// is verified in a headed browser (see the deploy notes).
test('a digit drawn in the panel is classified by ai.nn', async ({ page }) => {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'mnist.rox');
    });
    page.on('pageerror', e => console.error('[pageerror]', e.message));
    page.on('console', m => { if (m.type() === 'error') console.error('[console]', m.text()); });
    await page.goto('/');

    // The script parks in web.serve(), so the panel appears before any
    // inference has run.
    const canvas = page.locator('.mnist-canvas');
    await expect(canvas).toBeVisible({ timeout: 60_000 });
    await expect(page.locator('.output-pane .out'))
        .toContainText(/inference running on: (cpu|webgpu)/);
    await expect(page.locator('.mnist-digit')).toHaveText('–');

    // Draw a vertical stroke down the middle: a "1".
    const box = await canvas.boundingBox();
    const x = box.x + box.width / 2;
    await page.mouse.move(x, box.y + box.height * 0.15);
    await page.mouse.down();
    await page.mouse.move(x, box.y + box.height * 0.85, { steps: 25 });
    await page.mouse.up();

    // First inference also fetches ort's ~26MB runtime .wasm.
    await expect(page.locator('.mnist-digit')).toHaveText('1', { timeout: 60_000 });
    await expect(page.locator('.mnist-bar.win span')).toHaveText('1');
    await expect(page.locator('.mnist-device')).toHaveText(/GPU \(WebGPU\)|CPU/);
    await expect(page.locator('.mnist-hint')).toContainText('inferences');

    // Clear resets the verdict through the store, not just the canvas.
    await page.getByRole('button', { name: 'clear' }).click();
    await expect(page.locator('.mnist-digit')).toHaveText('–');
});

// The app pane picks its panel by which store was defined most recently, and
// that ranking used to be a PER-STORE define count: a store defined twice
// outranked one defined once regardless of which script was live, so opening
// mnist while tanks was running left the tanks view on screen (a reload
// "fixed" it, because then each store had been defined once). Switching back
// and forth is the test -- one switch passes even with the broken ranking.
test('the app panel follows the file you open', async ({ page }) => {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'tanks.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tanks-readout, .tanks3d-canvas').first())
        .toBeVisible({ timeout: 60_000 });

    const open = async name => {
        await page.locator('.menu summary').click();
        await page.locator('.menu-items button', { hasText: name }).first().click();
    };

    await open('mnist.rox');
    await expect(page.locator('.mnist-canvas')).toBeVisible({ timeout: 60_000 });
    await open('oven.rox');
    await expect(page.locator('.panel .readout')).toBeVisible({ timeout: 60_000 });
    await open('mnist.rox');
    await expect(page.locator('.mnist-canvas')).toBeVisible({ timeout: 60_000 });
    await open('tanks.rox');
    await expect(page.locator('.tanks-readout, .tanks3d-canvas').first())
        .toBeVisible({ timeout: 60_000 });
});
