import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

// The two headers are not optional. Roxal's VM spawns pthreads (the GC reclaimer
// and the dataflow engine actor) during construction, Emscripten maps those onto
// Web Workers over a SharedArrayBuffer, and SharedArrayBuffer only exists in a
// cross-origin-isolated document. Without these the page loads and the VM cannot
// be constructed at all.
//
// This replaces the hand-rolled serve.py for app development; the same two
// headers go on whatever serves the app in production.
const crossOriginIsolation = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
};

export default defineConfig({
    plugins: [react()],
    // Relative asset paths: the Electron shell loads dist/aistudio.html from
    // the filesystem, and a hosted deploy may mount the pages under a prefix.
    base: './',
    // The wire codec (wasm/roxal-wire.js) is imported from the repo, one
    // directory up: single-sourced with the wasm glue, so allow serving it.
    server: { headers: crossOriginIsolation, fs: { allow: [resolve(here, '..')] } },
    preview: { headers: crossOriginIsolation },
    // roxal.js is an Emscripten MODULARIZE bundle, not an ES module: it is loaded
    // from public/ with a script tag and hands us a global factory. Keeping it out
    // of the bundler is deliberate -- Vite must not try to rewrite its worker
    // spawning or its .wasm/.data fetches.
    optimizeDeps: { exclude: ['roxal'] },
    // Two pages from one project: the IDE at / and AI Studio at /aistudio.html
    // (a later deploy may mount them as sibling paths of one domain).
    build: {
        rollupOptions: {
            input: {
                main: resolve(here, 'index.html'),
                aistudio: resolve(here, 'aistudio.html'),
            },
        },
    },
});
