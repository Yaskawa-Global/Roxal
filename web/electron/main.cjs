// AI Studio as a desktop app.
//
// Electron is only the window and the native menus here. The VM is the same
// native process the browser page talks to -- `roxal --web-host` -- spawned
// by this file on an ephemeral port, and the page is the built
// dist/aistudio.html pointed at it. Nothing in the page knows it is inside
// Electron except the menu bridge in preload.cjs.
//
//   npm run app -- [workspace-dir | diagram.rox]   (from web/; default examples/aistudio)
//   AISTUDIO_DEV_URL=http://localhost:5173 npm run app     (hack on the UI with HMR)
//
// Environment: ROXAL_BIN (default ../build/roxal), ROXAL_MODULES (default
// ../modules), AISTUDIO_ROOT (the workspace when no argument is given).

'use strict';

const { app, BrowserWindow, Menu, dialog, ipcMain } = require('electron');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const repo = path.resolve(__dirname, '..', '..');
const roxalBin = process.env.ROXAL_BIN || path.join(repo, 'build', 'roxal');
const modulesDir = process.env.ROXAL_MODULES || path.join(repo, 'modules');
const distPage = path.join(repo, 'web', 'dist', 'aistudio.html');
const settingsPath = () => path.join(app.getPath('userData'), 'settings.json');

function readSettings() {
    try { return JSON.parse(fs.readFileSync(settingsPath(), 'utf8')); } catch { return {}; }
}
function writeSettings(s) {
    try { fs.mkdirSync(path.dirname(settingsPath()), { recursive: true }); fs.writeFileSync(settingsPath(), JSON.stringify(s, null, 2)); }
    catch (e) { console.warn('aistudio: could not save settings:', e.message); }
}

// Where the app starts: a workspace directory (the host's --root) and,
// optionally, the diagram inside it to open. The argument may name either --
// a .rox file makes its folder the workspace and opens that file.
function initialTarget() {
    const arg = process.argv.slice(app.isPackaged ? 1 : 2).find(a => !a.startsWith('-'));
    let file = null;
    let wanted = arg;
    if (arg && fs.existsSync(arg) && fs.statSync(arg).isFile()) {
        wanted = path.dirname(arg);
        file = path.basename(arg);
    }
    const candidates = [wanted, process.env.AISTUDIO_ROOT, readSettings().workspace,
                        path.join(repo, 'examples', 'aistudio')];
    for (const c of candidates)
        if (c && fs.existsSync(c) && fs.statSync(c).isDirectory())
            return { root: path.resolve(c), file };
    return { root: path.join(repo, 'examples', 'aistudio'), file };
}

// ---------------------------------------------------------------- the VM host
let host = null;          // { proc, port, root }

function startHost(root) {
    return new Promise((resolve, reject) => {
        if (!fs.existsSync(roxalBin)) {
            reject(new Error(`no roxal binary at ${roxalBin} -- build it (cmake --build build/) or set ROXAL_BIN`));
            return;
        }
        const proc = spawn(roxalBin, ['--web-host', '--web-port', '0', '-p', modulesDir, '--root', root],
                           { cwd: repo, stdio: ['ignore', 'pipe', 'pipe'] });
        let out = '';
        let settled = false;
        proc.stdout.on('data', d => {
            out += d;
            const m = out.match(/listening on ws:\/\/127\.0\.0\.1:(\d+)/);
            if (m && !settled) {
                settled = true;
                host = { proc, port: Number(m[1]), root };
                resolve(host);
            }
            process.stdout.write(d);
        });
        proc.stderr.on('data', d => process.stderr.write(d));
        proc.on('exit', code => {
            if (!settled) { settled = true; reject(new Error(`the Roxal host exited (code ${code}) before listening:\n${out}`)); }
            if (host && host.proc === proc) host = null;
        });
    });
}

function stopHost() {
    if (!host) return;
    const h = host;
    host = null;
    try { h.proc.kill('SIGTERM'); } catch { /* gone */ }
}

// ---------------------------------------------------------------- the window
let win = null;

// The page is told which diagram to open at boot through its ?file= query
// (the last-opened one otherwise): that is how "Open Diagram…" from another
// folder lands on the chosen file -- the page load resolves only after
// did-finish-load, so nothing registered after it would ever fire.
function pageUrl(file) {
    const dev = process.env.AISTUDIO_DEV_URL;
    if (!dev) return null;
    const q = new URLSearchParams({ host: `ws://127.0.0.1:${host.port}` });
    if (file) q.set('file', file);
    return `${dev.replace(/\/$/, '')}/aistudio.html?${q}`;
}

async function loadPage(file) {
    const url = pageUrl(file);
    if (url) { await win.loadURL(url); return; }
    if (!fs.existsSync(distPage)) {
        await win.loadURL('data:text/html,' + encodeURIComponent(
            `<body style="font:14px sans-serif;padding:2rem;background:#14161a;color:#d8dee9">
             <h2>AI Studio: no built page</h2>
             <p>Build it first: <code>cd web &amp;&amp; npm run build</code>, then relaunch.</p>
             <p>Or set <code>AISTUDIO_DEV_URL=http://localhost:5173</code> with <code>npm run dev</code> running.</p></body>`));
        return;
    }
    const query = { host: `ws://127.0.0.1:${host.port}` };
    if (file) query.file = file;
    await win.loadFile(distPage, { query });
}

function send(action) {
    if (win && !win.isDestroyed()) win.webContents.send('menu', action);
}

async function switchWorkspace(root, file) {
    stopHost();
    try {
        await startHost(root);
    } catch (e) {
        dialog.showErrorBox('AI Studio', String(e.message || e));
        return;
    }
    const s = readSettings(); s.workspace = root; writeSettings(s);
    win.setTitle(`AI Studio — ${root}`);
    await loadPage(file);
}

function buildMenu() {
    const template = [
        {
            label: 'File',
            submenu: [
                {
                    label: 'Open Workspace Folder…', accelerator: 'CmdOrCtrl+Shift+O',
                    click: async () => {
                        const r = await dialog.showOpenDialog(win, {
                            title: 'Workspace folder (its .rox diagrams and models/)',
                            defaultPath: host?.root, properties: ['openDirectory'] });
                        if (!r.canceled && r.filePaths[0]) await switchWorkspace(r.filePaths[0]);
                    },
                },
                {
                    label: 'Open Diagram…', accelerator: 'CmdOrCtrl+O',
                    click: async () => {
                        const r = await dialog.showOpenDialog(win, {
                            title: 'Open a diagram from the workspace', defaultPath: host?.root,
                            filters: [{ name: 'Roxal', extensions: ['rox'] }], properties: ['openFile'] });
                        if (r.canceled || !r.filePaths[0]) return;
                        const file = r.filePaths[0];
                        const dir = path.dirname(file);
                        if (host && path.resolve(dir) !== path.resolve(host.root)) {
                            // a file outside the workspace: make its folder the
                            // workspace and boot the page on this file
                            await switchWorkspace(dir, path.basename(file));
                        } else {
                            send({ kind: 'open-file', name: path.basename(file) });
                        }
                    },
                },
                { type: 'separator' },
                { label: 'New Diagram…', accelerator: 'CmdOrCtrl+N', click: () => send({ kind: 'new' }) },
                { label: 'Save', accelerator: 'CmdOrCtrl+S', click: () => send({ kind: 'save' }) },
                { label: 'Save As…', accelerator: 'CmdOrCtrl+Shift+S', click: () => send({ kind: 'saveAs' }) },
                { type: 'separator' },
                { label: 'Reload Page', accelerator: 'CmdOrCtrl+R', click: () => loadPage() },
                { role: 'quit' },
            ],
        },
        {
            label: 'Run',
            submenu: [
                { label: 'Run Diagram', accelerator: 'F5', click: () => send({ kind: 'run' }) },
                { label: 'Stop', accelerator: 'Shift+F5', click: () => send({ kind: 'stop' }) },
            ],
        },
        {
            label: 'View',
            submenu: [
                { label: 'Toggle Console', accelerator: 'CmdOrCtrl+J', click: () => send({ kind: 'toggle-console' }) },
                { label: 'Diagram', accelerator: 'CmdOrCtrl+1', click: () => send({ kind: 'view', view: 'diagram' }) },
                { label: 'Source', accelerator: 'CmdOrCtrl+2', click: () => send({ kind: 'view', view: 'source' }) },
                { type: 'separator' },
                { role: 'togglefullscreen' },
                { role: 'toggleDevTools' },
                { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' },
            ],
        },
    ];
    Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

async function createWindow() {
    win = new BrowserWindow({
        width: 1500, height: 950,
        backgroundColor: '#14161a',
        title: 'AI Studio',
        webPreferences: {
            preload: path.join(__dirname, 'preload.cjs'),
            contextIsolation: true,
            nodeIntegration: false,
        },
    });
    buildMenu();
    const { root, file } = initialTarget();
    try {
        await startHost(root);
    } catch (e) {
        dialog.showErrorBox('AI Studio: cannot start the Roxal VM', String(e.message || e));
        app.quit();
        return;
    }
    win.setTitle(`AI Studio — ${root}`);
    await loadPage(file);
}

ipcMain.handle('aistudio:workspace', () => host?.root ?? null);

app.whenReady().then(createWindow);
app.on('window-all-closed', () => { stopHost(); app.quit(); });
app.on('before-quit', stopHost);
