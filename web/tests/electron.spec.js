import { test, expect, _electron as electron } from '@playwright/test';
import { mkdtempSync, existsSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
// The desktop shell: Electron spawns the native host on an ephemeral port,
// loads the built page from the filesystem against it, and its native menu
// drives the page through the preload bridge. Same page, same VM, no browser.

const repo = resolve(import.meta.dirname, '..', '..');
const roxalBin = process.env.ROXAL_BIN || join(repo, 'build', 'roxal');

test.skip(!existsSync(roxalBin), `no native roxal at ${roxalBin} (set ROXAL_BIN)`);
test.skip(!existsSync(join(repo, 'web', 'dist', 'aistudio.html')), 'no built page (npm run build)');

test('the Electron shell boots the VM, loads the page from disk, and its menu reaches the page', async () => {
    const root = mkdtempSync(join(tmpdir(), 'roxal-electron-'));
    copyFileSync(join(repo, 'web', 'src', 'demos', 'cosine.rox'), join(root, 'cosine.rox'));
    copyFileSync(join(repo, 'web', 'src', 'demos', 'flipflop.rox'), join(root, 'flipflop.rox'));

    // An IDE hosting this test run (VS Code, Claude Code) exports
    // ELECTRON_RUN_AS_NODE=1, which turns Electron into plain node.
    const env = { ...process.env, ROXAL_BIN: roxalBin };
    delete env.ELECTRON_RUN_AS_NODE;
    // The argument names a DIAGRAM here, not the folder: its directory
    // becomes the workspace and the app opens that file.
    const app = await electron.launch({
        args: [join(repo, 'web', 'electron', 'main.cjs'), join(root, 'flipflop.rox')],
        cwd: join(repo, 'web'),
        env,
    });
    try {
        const page = await app.firstWindow();
        const errors = [];
        page.on('pageerror', e => errors.push('pageerror: ' + e.message));

        // Loaded from the filesystem, talking to the spawned host.
        expect(page.url()).toMatch(/^file:.*aistudio\.html\?host=ws%3A%2F%2F127\.0\.0\.1%3A\d+/);
        await expect(page.locator('.ais-file')).toHaveText(/flipflop\.rox/, { timeout: 90000 });
        await expect(page.locator('.ais-status')).toContainText('native VM', { timeout: 30000 });
        await expect(page.locator('.df-node').first()).toBeVisible({ timeout: 60000 });
        console.log('ELECTRON BOOT ok', page.url());

        // The source view (Monaco, workers loaded from file://) renders.
        await page.locator('.view-toggle button', { hasText: 'source' }).click();
        await expect.poll(async () =>
            page.evaluate(() => window.monaco?.editor.getEditors()[0]?.getModel()?.getValue() ?? ''),
            { timeout: 30000 }).toContain('@dataflow_diagram');
        await page.locator('.view-toggle button', { hasText: 'diagram' }).click();
        console.log('ELECTRON SOURCE ok');

        // Native menu actions arrive through the preload bridge. (The drawer
        // state persists in Electron's user data between runs, so toggle
        // relative to whatever it is now.)
        const drawerWas = await page.locator('.ais-drawer-body').count();
        await app.evaluate(({ BrowserWindow }) => {
            BrowserWindow.getAllWindows()[0].webContents.send('menu', { kind: 'toggle-console' });
        });
        await expect(page.locator('.ais-drawer-body')).toHaveCount(drawerWas ? 0 : 1, { timeout: 10000 });
        await app.evaluate(({ BrowserWindow }) => {
            BrowserWindow.getAllWindows()[0].webContents.send('menu', { kind: 'open-file', name: 'cosine.rox' });
        });
        await expect(page.locator('.ais-file')).toHaveText(/cosine\.rox/, { timeout: 30000 });
        await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 30000 });
        console.log('ELECTRON MENU ok');

        expect(errors).toEqual([]);
    } finally {
        await app.close();
    }
});
