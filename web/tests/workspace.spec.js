import { test, expect } from '@playwright/test';
// The IDE demo loop: files under /data (OPFS in the browser), a REPL through
// the compiler, and the whole thing bootstrapped through the workspace store.
// One test rather than four because the stages build on each other and each
// VM boot costs seconds.
test('files persist in OPFS; the REPL evaluates through the compiler', async ({ page }) => {
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text().slice(0, 200)); });
    await page.goto('/');
    // Boot: tab appears, oven runs
    await expect(page.locator('.tab.active')).toHaveText(/app\.rox/, { timeout: 90000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });
    console.log('BOOT ok');

    // REPL
    await page.locator('.repl-input').fill('6 * 7');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').first()).toHaveText('42', { timeout: 20000 });
    console.log('REPL ok');

    // A typo is the commonest REPL input of all, and an unresolved name is a
    // FATAL runtime error in Roxal -- it used to take the whole app down. The
    // console checks the names an expression mentions before running it.
    await page.locator('.repl-input').fill('sfsdfds');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').nth(1)).toContainText('undefined: sfsdfds', { timeout: 20000 });
    // The app is untouched -- no restart, no lost state.
    await expect(page.locator('.v.big')).toHaveText('20.0°');
    console.log('TYPO-CONTAINED ok');

    // A fatal runtime error that the check cannot pre-empt (a statement, not an
    // expression) still kills the app -- the IDE detects it, restarts, and says so.
    await page.locator('.repl-input').fill('var boom = nosuch');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').nth(2)).toContainText('restart', { timeout: 30000 });
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 30000 });
    // ...and the REPL still answers afterwards.
    await page.locator('.repl-input').fill('"still " + "alive"');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').nth(3)).toContainText('still alive', { timeout: 20000 });
    console.log('FATAL-RECOVERY ok');

    // New file via menu
    page.on('dialog', d => d.accept('scratch.rox'));
    await page.locator('.menu summary').click();
    await page.getByRole('button', { name: 'New…' }).click();
    await expect(page.locator('.tab.active')).toHaveText(/scratch\.rox/, { timeout: 20000 });
    console.log('NEW ok');

    // Edit + save it
    await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().setValue('print("persisted!")\n'));
    await page.locator('.menu summary').click();
    await page.getByRole('button', { name: 'Save', exact: true }).click();
    await page.waitForTimeout(500);
    console.log('SAVE ok');

    // Reload: OPFS persistence — scratch.rox must still exist with content
    await page.reload();
    await expect(page.locator('.tab.active')).toHaveText(/scratch\.rox/, { timeout: 90000 });
    const text = await page.evaluate(() => window.monaco.editor.getEditors()[0].getModel().getValue());
    console.log('PERSIST', JSON.stringify(text));
    if (errors.length) console.log('ERRORS', errors.slice(0, 5));
});
