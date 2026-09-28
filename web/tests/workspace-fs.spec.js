import { test, expect } from '@playwright/test';
// The rooted file service of the IDE's "workspace" store (web.Workspace), in a
// real browser, where /data is OPFS. That is what the node suites cannot
// reach: OPFS cannot move a directory (fileio copies, then deletes), WASMFS
// does not keep file times (mtime is nil), replacing a file is
// remove-then-move, and a file another writer holds open is locked.

async function boot(page, query = '') {
    await page.goto('/' + query);
    await expect(page.locator('.tab.active')).toBeVisible({ timeout: 90000 });
    await page.waitForFunction(() => {
        const ws = window.__rox?.roxalStore?.('workspace');
        return !!ws && ws.methods.includes('write');
    }, null, { timeout: 60000 });
}

// One workspace store call, made in the page. Bounded: a hang is a failure
// of its own, not a test timeout with no information.
function ws(page, method, ...args) {
    return page.evaluate(([m, a]) => Promise.race([
        window.__rox.roxalStore('workspace').call(m, ...a),
        new Promise(r => setTimeout(() => r({ error: 'HUNG' }), 15000)),
    ]), [method, args]);
}

test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
        if (!localStorage.getItem('roxal-ide-last-file'))
            localStorage.setItem('roxal-ide-last-file', 'oven.rox');
    });
});

test('rename, atomic write, conflicts and confinement on OPFS', async ({ page }) => {
    await boot(page);
    // The app auto-runs on boot; let it settle so its restart does not swap
    // the store out from under the calls below.
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });
    await ws(page, 'remove', 'wsfs', true);            // leftovers of an earlier run

    const w1 = await ws(page, 'write', 'wsfs/a.rox', 'one');
    expect(w1.tag).toMatch(/^[0-9a-f]+-3$/);
    const r1 = await ws(page, 'read', 'wsfs/a.rox');
    expect(r1).toEqual({ text: 'one', tag: w1.tag });
    const st = await ws(page, 'stat', 'wsfs/a.rox');
    console.log('stat on OPFS:', JSON.stringify(st));
    expect(st.kind).toBe('file');
    expect(st.mtime).toBeNull();                      // WASMFS invents OPFS file times

    // Conflict detection by content tag.
    const w2 = await ws(page, 'write', 'wsfs/a.rox', 'two', r1.tag);
    expect(w2.tag).not.toBe(r1.tag);
    const c = await ws(page, 'write', 'wsfs/a.rox', 'three', r1.tag);
    expect(c).toMatchObject({ error: 'CONFLICT', tag: w2.tag });
    expect((await ws(page, 'read', 'wsfs/a.rox')).text).toBe('two');

    // A file rename is an OPFS move.
    expect(await ws(page, 'rename', 'wsfs/a.rox', 'wsfs/b.rox')).toEqual({});
    expect(await ws(page, 'stat', 'wsfs/a.rox')).toBeNull();
    expect((await ws(page, 'read', 'wsfs/b.rox')).text).toBe('two');

    // A directory rename cannot be an OPFS move: fileio copies, then deletes.
    await ws(page, 'mkdir', 'wsfs/dir/sub');
    await ws(page, 'write', 'wsfs/dir/sub/x.rox', 'X');
    await ws(page, 'write', 'wsfs/dir/y.rox', 'Y');
    const rd = await ws(page, 'rename', 'wsfs/dir', 'wsfs/dir2');
    console.log('directory rename on OPFS:', JSON.stringify(rd));
    expect(rd).toEqual({});
    expect(await ws(page, 'stat', 'wsfs/dir')).toBeNull();
    const tree = (await ws(page, 'list', 'wsfs', true)).map(e => e.path);
    expect(tree).toEqual(['wsfs/dir2', 'wsfs/dir2/sub', 'wsfs/dir2/sub/x.rox',
                          'wsfs/dir2/y.rox', 'wsfs/b.rox']);
    expect((await ws(page, 'read', 'wsfs/dir2/sub/x.rox')).text).toBe('X');

    // An atomic replace of an existing file; no temporary sibling survives.
    const w3 = await ws(page, 'write', 'wsfs/b.rox', 'final');
    expect((await ws(page, 'read', 'wsfs/b.rox')).text).toBe('final');
    expect((await ws(page, 'list', 'wsfs')).map(e => e.path)).toEqual(['wsfs/dir2', 'wsfs/b.rox']);

    expect((await ws(page, 'copy', 'wsfs/dir2', 'wsfs/dir3'))).toEqual({});
    expect((await ws(page, 'read', 'wsfs/dir3/y.rox')).text).toBe('Y');

    // Confined to the root.
    expect((await ws(page, 'read', '../x')).error).toBe('EINVAL');
    expect((await ws(page, 'read', '/etc/x')).error).toBe('EINVAL');
    expect((await ws(page, 'write', '../escape.rox', 'x')).error).toBe('EINVAL');

    // It all persists across a reload.
    await boot(page);
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });
    expect(await ws(page, 'read', 'wsfs/b.rox')).toEqual({ text: 'final', tag: w3.tag });
    expect((await ws(page, 'list', 'wsfs', true)).map(e => e.path)).toEqual([
        'wsfs/dir2', 'wsfs/dir2/sub', 'wsfs/dir2/sub/x.rox', 'wsfs/dir2/y.rox',
        'wsfs/dir3', 'wsfs/dir3/sub', 'wsfs/dir3/sub/x.rox', 'wsfs/dir3/y.rox',
        'wsfs/b.rox']);

    expect(await ws(page, 'remove', 'wsfs', true)).toEqual({});
});

test('a file held open for writing is refused with EACCES, not a hang', async ({ page }) => {
    await boot(page);
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });
    // A script that keeps a write handle open: under OPFS that is an
    // exclusive lock on the file.
    await ws(page, 'write', 'wsfsholder.rox', [
        'import fileio',
        'import web',
        "var h = fileio.open('/data/wsfs-locked.txt', write=true)",
        "fileio.write(h, 'held')",
        'fileio.flush(h)',
        "print('holding')",
        'web.serve()',
        '',
    ].join('\n'));
    await boot(page, '?file=wsfsholder');
    await expect(page.locator('pre.out')).toContainText('holding', { timeout: 60000 });

    const write = await ws(page, 'write', 'wsfs-locked.txt', 'x');
    const remove = await ws(page, 'remove', 'wsfs-locked.txt');
    const rename = await ws(page, 'rename', 'wsfs-locked.txt', 'wsfs-moved.txt');
    const read = await ws(page, 'read', 'wsfs-locked.txt');
    console.log('locked file:', JSON.stringify({ write, remove, rename, read }));
    expect(write.error).toBe('EACCES');
    expect(remove.error).toBe('EACCES');
    expect(rename.error).toBe('EACCES');

    // Back to an ordinary app, which releases the lock; then tidy up.
    await page.evaluate(() => localStorage.setItem('roxal-ide-last-file', 'oven.rox'));
    await boot(page, '?file=oven');
    await expect(page.locator('.v.big')).toHaveText('20.0°', { timeout: 60000 });
    await ws(page, 'remove', 'wsfs-locked.txt');
    await ws(page, 'remove', 'wsfsholder.rox');
});
