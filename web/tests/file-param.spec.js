import { test, expect } from '@playwright/test';

// ?file= picks the file the IDE opens at boot, ahead of the remembered file
// and the tanks.rox default -- so a link can point at a specific example.
test.describe('?file= URL parameter', () => {
    test('opens the named file, with or without the extension', async ({ page }) => {
        test.setTimeout(180000);
        // A remembered file the parameter has to override.
        await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'tanks.rox'));
        await page.goto('/?file=counter4');
        await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 120000 });

        await page.goto('/?file=counter4.rox');
        await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 120000 });
    });

    test('an unknown name falls back to the remembered file', async ({ page }) => {
        test.setTimeout(180000);
        await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'tanks.rox'));
        await page.goto('/?file=no-such-example');
        await expect(page.locator('.tab.active')).toHaveText(/tanks\.rox/, { timeout: 120000 });
    });

    test('no parameter keeps the remembered file', async ({ page }) => {
        test.setTimeout(180000);
        await page.addInitScript(() => localStorage.setItem('roxal-ide-last-file', 'counter4.rox'));
        await page.goto('/');
        await expect(page.locator('.tab.active')).toHaveText(/counter4\.rox/, { timeout: 120000 });
    });
});
