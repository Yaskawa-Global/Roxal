import { test, expect } from '@playwright/test';

// The IDE's liveness invariant: something must always be parked, because the
// host loop is only pumped from the VM's dispatch loop. A user script that
// ENDS (a batch script, or one that errors before web.serve()) used to leave
// the IDE permanently dead -- Run hung on its own first store call, and the
// File menu stopped responding, surviving even a reload.
test('the IDE survives a script that ends without parking', async ({ page }) => {
    await page.addInitScript(() => {
        // Only when unset: this runs on EVERY navigation, so assigning
        // unconditionally would also override what the app itself remembered
        // and make a reload reopen the wrong file.
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/oven\.rox/, { timeout: 90000 });
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

// The same invariant, but on the BOOT path: the remembered file is a batch
// script, so it completes during boot and leaves nothing parked.  Boot's own
// recovery has to put the services back -- and it never could, because it
// runs inside one long async function started on the FIRST render, so every
// helper it calls sees that render's `rox`, which is null.  The recovery threw
// a TypeError instead, and the app panel showed it.
test('the IDE survives BOOTING into a script that ends', async ({ page }) => {
    test.setTimeout(180000);
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
    await page.goto('/');
    await expect(page.locator('.tab.active')).toHaveText(/oven\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });

    // Make the remembered file a script that runs and ends, and save it.
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue(
        ['import web', "print('batch done')", ''].join('\n')));
    await page.getByRole('button', { name: /^Run$/ }).click();
    await expect(page.locator('.pane-body.out')).toContainText('batch done', { timeout: 60000 });

    // Boot into it.  The program's own output arrives, and boot recovers.
    await page.reload();
    await expect(page.locator('.pane-body.out')).toContainText('batch done', { timeout: 90000 });
    await expect(page.locator('pre.error')).toHaveCount(0);

    // Services restored: the console evaluates through the compiler again.
    await expect(async () => {
        await page.locator('.repl-input').fill('6 * 7');
        await page.locator('.repl-input').press('Enter');
        await expect(page.locator('.repl-log')).toContainText('42', { timeout: 4000 });
    }).toPass({ timeout: 60000, intervals: [3000] });
});
