// Copy the wasm build artifacts into public/ so Vite serves them.
//
// This IS the boundary between the two build systems. CMake owns the C++ and
// produces roxal.js/.wasm/.data; the JS toolchain owns the app and consumes them
// as prebuilt inputs. Deliberately a file copy rather than CMake driving npm or
// npm driving CMake -- wrapping one build system in the other is how you get
// stale-dependency bugs that neither tool can see.
//
//   npm run sync-wasm     (also runs automatically before dev/build)

import { existsSync, mkdirSync, copyFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..', '..');
const from = process.env.ROXAL_WASM_DIST || join(repo, 'build-wasm-mt', 'dist');
const to = join(here, '..', 'public');

const ARTIFACTS = ['roxal.js', 'roxal.wasm', 'roxal.data'];

if (!existsSync(from)) {
    console.error(`sync-wasm: no wasm build at ${from}`);
    console.error('           build it first:  ./wasm/build.sh   (see wasm/README.md)');
    console.error('           or point ROXAL_WASM_DIST at an existing dist directory.');
    process.exit(1);
}

mkdirSync(to, { recursive: true });

let copied = 0;
for (const name of ARTIFACTS) {
    const src = join(from, name);
    if (!existsSync(src)) {
        console.error(`sync-wasm: missing ${src} -- is the wasm host built?`);
        process.exit(1);
    }
    const dst = join(to, name);
    // Copy when the source is newer, so `npm run dev` after a C++ rebuild picks
    // the new binary up without anyone remembering to.
    if (!existsSync(dst) || statSync(src).mtimeMs > statSync(dst).mtimeMs) {
        copyFileSync(src, dst);
        copied++;
    }
}

const mb = (statSync(join(to, 'roxal.wasm')).size / 1e6).toFixed(1);
console.log(`sync-wasm: ${copied ? copied + ' file(s) updated' : 'up to date'} `
          + `from ${from} (roxal.wasm ${mb} MB)`);
