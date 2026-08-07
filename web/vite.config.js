import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

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
    server: { headers: crossOriginIsolation },
    preview: { headers: crossOriginIsolation },
    // roxal.js is an Emscripten MODULARIZE bundle, not an ES module: it is loaded
    // from public/ with a script tag and hands us a global factory. Keeping it out
    // of the bundler is deliberate -- Vite must not try to rewrite its worker
    // spawning or its .wasm/.data fetches.
    optimizeDeps: { exclude: ['roxal'] },
});
