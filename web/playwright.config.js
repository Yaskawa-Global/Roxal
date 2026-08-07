import { defineConfig } from '@playwright/test';

// Browser tests for the Roxal web stack.
//
// These exist because the node suites structurally cannot see most of what can
// break here. Everything that went wrong on the first day of this work --
// a VM-thread trap on the no-argument park, page assets silently served stale,
// a backtick that killed an inline script, a signal property that never updated
// -- was invisible to node and obvious in a browser. This is the net for that.
//
//   npm run test:browser
//
// Uses the system Chrome (channel) rather than Playwright's own download: the
// browsers bundle is ~150MB and Chrome is already here. `npx playwright install
// chromium` if you would rather have the pinned build.
export default defineConfig({
    testDir: './tests',
    // A cold start compiles the app script in the VM; be patient but bounded.
    timeout: 90_000,
    expect: { timeout: 20_000 },
    fullyParallel: false,       // one VM, one port
    workers: 1,
    reporter: [['list']],
    use: {
        baseURL: 'http://localhost:4173',
        channel: 'chrome',
        // Chrome refuses to run as root without this in containers/CI.
        launchOptions: { args: ['--no-sandbox'] },
        trace: 'retain-on-failure',
    },
    // preview rather than dev: it serves the built bundle and needs no file
    // watcher, which matters where the inotify limit is low.
    webServer: {
        command: 'npm run build && npx vite preview --port 4173 --strictPort',
        url: 'http://localhost:4173',
        reuseExistingServer: !process.env.CI,
        timeout: 180_000,
    },
});
