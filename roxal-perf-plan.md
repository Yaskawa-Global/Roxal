# Roxal performance for pixel-scale work — findings & plan

Goal: pure-Roxal real-time framebuffer work (target: Doom, 320x200 @ 35 fps)
without a native game module. Measured on branch `qtfb`, 2026-07-18.

## Finding 1: the dev build was Debug — a 12-15x tax

`build/` was configured `CMAKE_BUILD_TYPE=Debug` (no optimization, asserts on).
All earlier "Roxal is slow" numbers measured that. `build-rel/` (Release, -O3)
now exists alongside:

| Workload (per frame/tick)      | Debug   | Release | ratio |
|--------------------------------|---------|---------|-------|
| 96x64 plasma, per-pixel loops  | 643 ms  | 51.7 ms | 12.4x |
| 48x32 palette-cycle loop       | 74 ms   | 4.9 ms  | 15x   |

Effective interpreter rate: ~300k ops/s (Debug) → **~4.5M ops/s (Release)**.
Use `build-rel/roxal` for anything perf-sensitive; keep `build/` for debugging
(runtests still uses it — everything passes under both).

## Finding 2: where Release time goes (callgrind, palette-cycle inner loop)

- `VM::execute` dispatch: 19%
- Value lifecycle (copy/dtor/assign/type()/asInt/refcount): ~24%
- **Event + continuation polling per instruction: ~13%**
  (`processEventDispatch` + lambda + `processContinuationDispatch`)
- Stack traffic (`Thread::push/pop/peek`, `VM::pop`): ~8%
- malloc/free (index vectors etc.): ~3%
- The *useful* work (e.g. `ObjTensor::setIndex`): **~1-3%**

i.e. a per-pixel Roxal loop is ~97% interpreter overhead — data-parallel work
must move into bulk ops (below); the interpreter items are second-order.

## Done this round: bulk tensor ops (the framebuffer enablers)

Elementwise `+ - * /` on tensors already existed (t⊕t, t⊕scalar). Added:

- **`table.take(indices)`** — gather rows along axis 0: table `[N, ...rest]`,
  integer-dtype indices of any shape → `indices.shape ++ rest`, table's dtype.
  One call does palette/LUT maps ([256,3] palette + [H,W] indices → [H,W,3]
  image), class-id → color, texture row lookups. Bounds-checked up front
  (fail-loud, and the copy loop stays check-free).
- **`t rem scalar` / `t rem t`** — elementwise remainder (sign of dividend,
  like scalars); wraps palette phases.
- **`t.fill(v)`** — in-place clear/fill (memset fast path for uint8).

Tests: `tensor_take` (+`_err`); suite 739/740 (the 1 = known expected fail).
NB implementation gotcha: validate/throw **before** `newTensorObj()` — a throw
across a live newObj unique_ptr terminates (UnreleasedObj contract).

**Result: a full 320x200 plasma tick — `palette.take((pattern + shift) rem
256)` — runs in 4.7 ms** (Release; would be ~68 ms as per-pixel loops). The
qt example (`examples/qt/framebuffer.rox`) now animates 320x200 @ 30 fps with
CPU-only rendering, verified live. Doom-resolution blit + present is solved.

## Round 2 (2026-07-18, same day): items 1-4 IMPLEMENTED — measured results

1. **Event/continuation dispatch fast guards** — instead of an N-instruction
   cadence, the functions' own early-out flags are checked inline at the call
   site (identical conditions at identical frequency → zero added event
   latency, zero semantic risk); the calls only happen when a flag is set.
   Also hoisted `finalizeWaitSuspension`'s `active` gate.
2. **Typed dtype kernels** (`withTensorDType` in Object.h): elementwise
   `+ - * / rem` (t⊕t, t⊕scalar), `astype` (src×dst nested dispatch), `sum`,
   `min`, `max`, `fill`, and `take`'s index reads all dispatch the dtype ONCE
   and run tight typed loops (double-mediated math — bit-identical results;
   generic at()/setAt() loop remains for Float16/Bool/mixed-dtype).
3. **Span-based tensor indexing**: `t[y, x, c]` get/set reads indices in place
   off the VM value stack (`ObjTensor::index/setIndex(const Value*, size_t)`)
   — no heap vector per element access (previously TWO per read).
4. **`-march=x86-64-v2`** for Release/RelWithDebInfo on x86-64 hosts
   (`ROXAL_RELEASE_MARCH` cache var, empty to disable; arm64 unaffected).

| Benchmark (Release)               | before  | after   | change |
|-----------------------------------|---------|---------|--------|
| 320x200 vectorized tick (+,rem,take) | 4.70 ms | 0.28 ms | **17x** |
| 48x32 per-pixel palette loop      | 4.87 ms | 4.58 ms | ~6%    |
| 96x64 per-pixel plasma (sin-heavy)| 51.7 ms | 50.9 ms | ~2%    |
| Identical-workload instructions   | 3.62 G  | 3.12 G  | −14%   |

Honest read: the bulk-op path got the transformative win (0.28 ms for the
whole Doom-resolution blit pipeline ≈ 2% of a 35 fps frame budget). For
interpreter-bound loops the removed per-instruction overhead (−14% Ir) was
largely pipeline-hidden — wall time only improved ~6%. Suite: 739/740 both
build types (the 1 = known expected fail). Bonus fix: `divide()` threw over a
live newObj unique_ptr on zero divisors (process abort) — now pre-scans.

Also fixed en route (round 1): `qt_image_convert`/FrameView stack unaffected.

## Still open (parked)

5. Bigger interpreter surgery (register VM, NaN-boxed Values to cut the ~24%
   Value-lifecycle cost, inline caches) — **explicitly parked by David**; only
   revisit if game-logic-scale scripting proves too slow with bulk ops doing
   the pixel work. The Wolfenstein-style raycaster milestone is the decider.

### More bulk ops, added as real use cases demand
- `t.paste(src, at=[y, x])` rect blit; slice-assign writes
  (`t[a:b, c:d] = src` — tensor *read* slicing already exists).
- Elementwise math funcs on tensors (`sin(t)`, ...) in the math module.
- Comparison → mask tensors + `where(mask, a, b)` (sprite transparency
  without branches).
- Saturating u8 arithmetic variants for graphics compositing if needed.

## What this means for Doom

The display path is fully solved: game state → column/span composition via
take()/slices → palette map → `fb.present()` fits in a ~2-6 ms/frame budget at
35 fps. The open question is the *renderer inner loops* (BSP walk, column
u-stepping) in interpreted Roxal at ~4.5M ops/s — items 1-3 plus composing the
per-column work as `take()` gathers are what make that plausible. Prototype a
textured-column raycaster (Wolfenstein-style) as the next milestone before
committing to full Doom.
