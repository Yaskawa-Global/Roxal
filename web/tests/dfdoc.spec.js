import { test, expect } from '@playwright/test';
// The data-flow editor loop: a diagram file (dfdoc dialect) opens as a React
// Flow canvas, palette clicks become edit ops against the VM-side mirror-AST
// document, the source pane shows the regenerated dialect text, and Run
// executes the generated harness whose probes print to the output pane.
test('a diagram opens as a canvas; edits regenerate source; Run probes outputs', async ({ page }) => {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'cosine.rox');
    });
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    await page.goto('/');

    // Boot: the diagram tab is active and the canvas shows its three nodes
    // (signal wire, cos node, output port).
    await expect(page.locator('.tab.active')).toHaveText(/cosine\.rox/, { timeout: 90000 });
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 60000 });
    console.log('CANVAS ok');

    // Run the complete diagram: the harness instantiates the component and
    // its output probe prints values.
    await page.locator('button.run').click();
    await expect(page.locator('.out')).toContainText('out = ', { timeout: 60000 });
    console.log('RUN ok');

    // Live values: the running instance's wires are sampled by provenance and
    // labeled onto the edges (the cosine iterate lives in (0, 1)).
    await expect(page.locator('.react-flow__edge-text').first()).toContainText('0.', { timeout: 30000 });
    console.log('LIVE ok');

    // Palette: math extracted VM-side from /stdlib/math.rox. The new node's
    // input is deliberately left unconnected — wiring it is edit-op work, and
    // an incomplete diagram must still regenerate source cleanly.
    await expect(page.locator('.df-palette button', { hasText: /^sin$/ })).toBeVisible({ timeout: 30000 });
    await page.locator('.df-palette button', { hasText: /^sin$/ }).click();
    await expect(page.locator('.df-node')).toHaveCount(4, { timeout: 20000 });
    console.log('ADD-NODE ok');

    // The op regenerated the source; the source view shows it (read-only).
    await page.locator('.view-toggle button', { hasText: 'source' }).click();
    await expect(page.locator('.editor')).toBeVisible();
    await expect.poll(async () =>
        page.evaluate(() => window.monaco?.editor.getEditors()[0]?.getModel()?.getValue() ?? ''),
        { timeout: 20000 }).toContain('math.sin');
    console.log('SOURCE ok');

    // Back to the canvas: the document round-trips through the view, and the
    // dangling sin input is flagged by check.
    await page.locator('.view-toggle button', { hasText: 'diagram' }).click();
    await expect(page.locator('.df-node')).toHaveCount(4);
    await expect(page.locator('.df-diag.df-diag-error')).toContainText('unconnected', { timeout: 20000 });
    console.log('DIAG ok');

    // Run is gated on check errors -- the wiring failure is reported up front
    // instead of a nil-conversion error mid-network.
    await page.locator('button.run').click();
    await expect(page.locator('.run-error')).toContainText('unconnected', { timeout: 30000 });
    console.log('RUN-GATED ok');

    // Select the dangling node and delete it via the palette button; the
    // diagram is clean again and runs.
    await page.locator('.df-node', { hasText: 'math.sin' }).click();
    await page.locator('.df-delete').click();
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 20000 });
    await expect(page.locator('.df-diag.df-diag-error')).toHaveCount(0);
    await page.locator('button.run').click();
    await expect(page.locator('.out')).toContainText('out = ', { timeout: 60000 });
    console.log('DELETE+RERUN ok');

    if (errors.length) console.log('ERRORS', errors.slice(0, 5));
});

// Composition: counter4.rox instantiates flipflop.rox four times. The canvas
// shows the instances, Run counts in binary, live values ride the wires, and
// double-clicking an instance drills into its file.
test('a composed diagram: flip-flops as nodes', async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 90000 });
    // 14 = 1 input + 4 flip-flop instances + 3 and-gates + pack4 + 5 outputs
    await expect(page.locator('.df-node')).toHaveCount(14, { timeout: 60000 });
    await expect(page.locator('.df-node.df-diagram')).toHaveCount(4);
    console.log('COMPOSED-CANVAS ok');

    await expect(page.locator('.df-palette .df-component', { hasText: 'FlipFlop' }))
        .toBeVisible({ timeout: 30000 });
    console.log('COMPONENTS ok');

    await page.locator('button.run').click();
    await expect(page.locator('.out')).toContainText('q1 = ', { timeout: 60000 });
    await expect(page.locator('.react-flow__edge-text').first())
        .toContainText(/true|false/, { timeout: 30000 });
    console.log('COMPOSED-RUN ok');

    await page.locator('.df-node.df-diagram').first().dblclick();
    await expect(page.locator('.tab.active')).toHaveText(/flipflop\.rox/, { timeout: 20000 });
    await expect(page.locator('.df-node')).toHaveCount(4, { timeout: 30000 });
    console.log('DRILL-IN ok');
});
