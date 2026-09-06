import { test, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
// The IDE against a NATIVE VM: `roxal --web-host` serves the same store
// protocol over a WebSocket, and the page selects it with ?host=ws://...
// Everything the wasm boot does -- seed the examples into the data directory,
// open the last file, run it, sample live values, answer the REPL -- must
// work unchanged, because nothing above the host interface knows which VM
// it got. This is milestone M1's acceptance test for the native host.

const repo = resolve(import.meta.dirname, '..', '..');
const roxalBin = process.env.ROXAL_BIN || join(repo, 'build', 'roxal');

test.skip(!existsSync(roxalBin), `no native roxal at ${roxalBin} (set ROXAL_BIN)`);

let host, port;
test.beforeAll(async () => {
    const root = mkdtempSync(join(tmpdir(), 'roxal-ide-native-'));
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

test('the IDE runs a diagram and the REPL against roxal --web-host', async ({ page }) => {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'cosine.rox');
    });
    const errors = [];
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    await page.goto(`/?host=ws://127.0.0.1:${port}`);

    // Boot over the socket: the bootstrap's build store reports the host's
    // root as the data directory, the examples are seeded there, and the
    // diagram opens as a canvas.
    await expect(page.locator('.tab.active')).toHaveText(/cosine\.rox/, { timeout: 90000 });
    await expect(page.locator('.df-node')).toHaveCount(3, { timeout: 60000 });
    const dataDir = await page.evaluate(() => window.__rox?.roxalStore('build').getSnapshot().data_dir);
    expect(dataDir).toMatch(/roxal-ide-native-/);
    console.log('NATIVE BOOT ok', dataDir);

    // Run: the harness runs in the native VM; its probe output arrives over
    // the control channel.
    await page.locator('button.run').click();
    await expect(page.locator('.df-output .df-text').first()).toContainText(/\d/, { timeout: 60000 });   // text outputs show on their node
    console.log('NATIVE RUN ok');

    // Live values sampled by provenance, labeled onto the edges.
    await expect(page.locator('.react-flow__edge-text').first()).toContainText('0.', { timeout: 30000 });
    console.log('NATIVE LIVE ok');

    // The palette is derived from the host's real module directory.
    await expect(page.locator('.df-palette button', { hasText: /^sin$/ })).toBeVisible({ timeout: 30000 });
    console.log('NATIVE PALETTE ok');

    // REPL through the workspace actor.
    await page.locator('.repl-input').fill('6 * 7');
    await page.locator('.repl-input').press('Enter');
    await expect(page.locator('.repl-out').first()).toContainText('42', { timeout: 20000 });
    console.log('NATIVE REPL ok');

    expect(errors).toEqual([]);
});
