import { test, expect } from '@playwright/test';

// The IDE's liveness invariant: something must always be parked, because the
// host loop is only pumped from the VM's dispatch loop. A user script that
// ENDS (a batch script, or one that errors before web.serve()) used to leave
// the IDE permanently dead -- Run hung on its own first store call, and the
// File menu stopped responding, surviving even a reload.
test('the IDE survives a script that ends without parking', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/app\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });

    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel()
        .setValue('print("just a batch script")\n'));
    await page.getByRole('button', { name: /^Run$/ }).click();

    const runBtn = page.getByRole('button', { name: /Run|running/ });
    await expect(runBtn).toHaveText('Run', { timeout: 30000 });
    // Reported as a note, not a failure: finishing is what a batch script does.
    await expect(page.locator('.run-note')).toContainText('ran to completion', { timeout: 10000 });

    // The IDE is still alive: Run works again...
    await page.getByRole('button', { name: /^Run$/ }).click();
    await expect(runBtn).toHaveText('Run', { timeout: 30000 });

    // ...the File menu still switches files...
    page.on('dialog', d => d.accept('after.rox'));
    await page.locator('.menu summary').click();
    await page.getByRole('button', { name: 'New…' }).click();
    await expect(page.locator('.tab.active')).toHaveText(/after\.rox/, { timeout: 20000 });

    // ...and the console still evaluates.
    await page.locator('.repl-input').fill('6 * 7');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').first()).toHaveText('42', { timeout: 20000 });
});
