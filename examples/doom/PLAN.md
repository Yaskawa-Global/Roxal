# Doom in Roxal — implementation plan

Branch `doom`. Goal: Doom running in **pure Roxal** in `examples/doom/`, self-contained,
displayed through the qt FrameView software-framebuffer path. "Pure Roxal" means the game
code is Roxal — no native game module. Generic engine improvements (tensor ops, fileio)
that the game reveals a need for are in scope and land as normal engine features with tests.

Transcribing OSS C source is fine (linuxdoom is GPL2 — mark this example GPL2-compatible;
assets are BSD via Freedoom/miniwad).

## References

| Purpose | Source |
|---|---|
| Ground truth | linuxdoom-1.10 / Chocolate Doom (`r_bsp.c`, `r_segs.c`, `r_plane.c`, `r_things.c`, `p_map.c`) |
| Rewrite structure in a GC'd OO language | Managed Doom (C#, github.com/sinshu/managed-doom) |
| Incremental milestone ordering | DIYDoom notes (github.com/amroibrahim/DIYDoom) |
| Renderer explanation | Fabien Sanglard, *Game Engine Black Book: DOOM* |
| Raycaster milestone | Lodev raycasting tutorial (lodev.org/cgtutor/raycasting.html) |

## Verified engine facts this plan builds on

- **Frame path** (proven in `examples/qt/framebuffer.rox`): index tensor → `palette.take(idx)`
  → `fb.present()`; `qt.set_render_backend("software")`; QML `Keys` → signal for input
  (down/up + autoRepeat verified); `qt.post_key` + `engine.grab_window()` for headless tests.
  ~0.28 ms/frame for the take-composition at 320×200 in Release.
- **Tensor surface today**: elementwise `+ - * / rem`; `take` (gather along first axis),
  `fill`, `astype(dtype, scale=)`, `min/max/sum`, `to_bytes`, `shape/dtype/dims`;
  **read**-slicing `t[a:b, ...]`. Missing: slice-assign/paste, arange, concat, select/where.
- **Perf envelope**: interpreted ~4.5M ops/s in Release. Benchmark ONLY with `build-rel/`
  (Debug is 12–15× slower). Pixels stay in bulk ops; interpreted loops are for per-column /
  per-object logic only.
- **fileio**: `read_file(path, format='binary')` → byte list (packed-byte-list fast path
  exists in the VM); `tensor(shape=, dtype=, data=list)` construction exists. Bitwise `&`/`|`
  exist; no shifts needed (mul/div by powers of two suffice).
- **Multi-file layout works**: the script's directory is appended to the module search
  paths (roxal.cpp:587), so `import wad` from `examples/doom/doom.rox` resolves to
  `examples/doom/wad.rox`.

## Up-front technical decisions (flag disagreement now, all reversible)

1. **Float, not 16.16 fixed-point.** Fixed-point exists for 1993 hardware and demo sync;
   we need neither. Transcribe Doom's *structure* (BSP walk, clipping algorithms) but do
   the math in float radians — drop `finesine`/`tantoangle`/BAM angles. Removes overflow
   fussiness; VM float math costs the same as int.
2. **320×200, 8-bit palette space, authentic light diminishing.** Renderer writes palette
   *indices* into a `[200,320]` int32 frame; lighting = `COLORMAP` gather
   (`colormap[light].take(idx)` per column/span — it composes with the texture gather);
   final present = `playpal.take(frame)`. Everything stays in the proven take-LUT recipe.
3. **Hybrid data layout.** Game logic uses Roxal type instances (readable, Managed-Doom
   style: `Sector`, `Line`, `Mobj`); bulk pixel data (textures, flats, colormaps, frame)
   stays in tensors. Map lumps are decoded from the WAD byte tensor with bulk ops
   (`lo + hi*256`, sign-fix `(v + 32768) rem 65536 - 32768`), then materialized into objects.
4. **Use the WAD's precomputed BSP** (SEGS/SSECTORS/NODES). No node building — same as
   every source port.
5. **35 Hz game tics decoupled from render**, like the original (`qt.every` drives render;
   tics accumulate on elapsed time).

## Milestones

### M0 — engine prep: tensor slice-assign (the one hard prerequisite) — DONE 2026-07-18

Landed: `collectSliceIndices` shared read/write range expansion; `setIndex` slice path
(tensor value must match slice shape exactly, number broadcasts; step/closed ranges work;
one COW + typed loop per assignment; self-assign safe). VM resolves futures/signals on
range-assign like Matrix. Tests: `tensor_slice_assign` (+2 err tests); full suite green.
Release benchmark of the raycaster column-blit composition
(`frame[y0:y1, x] = texcol.take((ramp[0:h]*step).astype("int32"))`, 320 columns ×200 rows):
**6.3 ms/frame** — inside the 33 ms/30 fps budget with ~27 ms left for interpreted DDA
and game logic. (Debug: 115 ms/frame — never benchmark with build/.)

The renderer is column-writes; today tensors only slice on read.

- `t[a:b, c] = <1D tensor or scalar>` and rectangular `t[a:b, c:d] = <tensor or scalar>`
  (shape-checked paste). This is the column/span blit primitive.
- Verify `take` gather semantics on the shapes we need: `[256,3].take([H,W])` (proven),
  `1D.take(1D)` (texture column sample), `[34,256]`-row take (colormap).
- Skip `arange`: precompute a `[200]` ramp tensor once at startup with a loop.
- Tests in `tests/` for slice-assign (shapes, dtype coercion, negative/clamped ranges).

Contingency (only if M1 shows GC pressure from ~1.6k small temporaries/frame): in-place
elementwise variants or an `out=` parameter. Don't build until measured.

### M1 — Wolfenstein raycaster: the perf gate (`raycaster.rox` + `.qml`) — DONE 2026-07-18

Landed: `raycast_core.rox` (map/textures/palette/DDA renderer/movement — importable, no
qt), `raycaster.rox` (interactive shell), `raycaster.qml`, `raycaster_smoke.rox` (5
headless checks, all pass), `raycaster_bench.rox`. Verified end-to-end on a virtual
display: textures/perspective/side-shading correct, WASD+arrows move/turn with collision,
Esc quits cleanly.

**Measured (Release, 320×200):** core render+palette 13.5 ms/frame — split: interpreted
DDA+loop 7.8 ms, per-column tensor ops ~5.6 ms (~4.5 µs/op dispatch on ~100-elem
tensors), palette map 0.10 ms. Interactive render+present ≈ 21 ms/frame (~45 fps
sustained) — present/scene path adds ~7 ms, worth investigating.

**Gate verdict: PROCEED.** 30 fps holds with ~1.5× headroom. Doom's wall pass runs
~2–3× the per-column logic, which projects to borderline — so (a) schedule the parked
interpreter work (register VM / NaN-boxing) in parallel with M2/M3, per the 15–33 ms
band; (b) cheap wins identified: small-tensor op dispatch overhead and the ~7 ms
present path. Roxal gotchas learned: `elseif` parses but is unimplemented (use `match`),
scientific notation literals (1e30) don't parse, `import math.*` exports `pi`.

Pure grid-map DDA raycaster per Lodev: 320 columns, textured walls, solid floor/ceiling
fills, WASD+arrow movement with grid collision, procedural 64×64 textures (no WAD yet).

Per column: interpreted DDA + scalar math (~60–100 ops), then bulk texture sample:
`tex_col.take((ramp[0:h] * step + offset).astype("int32"))` → slice-assign into the frame.
Estimated ~25k interpreted ops + ~1.6k small-tensor kernel dispatches per frame ≈ 8–12 ms.

**Acceptance:** sustained ≥30 fps at 320×200 in `build-rel/`, on-screen ms/frame counter,
headless smoke test (post_key a scripted walk, grab_window, assert wall colors).

**Gate:** record ms/frame. ≤15 ms → proceed to Doom renderer as planned. 15–33 ms →
proceed, but schedule the parked interpreter surgery (register VM / NaN-boxing) in
parallel. >33 ms → stop; do the interpreter work first. Doom's wall pass does roughly
2–3× the raycaster's per-column logic, so headroom matters.

### M2 — WAD library + asset viewer (`wad.rox`, `gfx.rox`, `viewer.rox`) — DONE 2026-07-18

Landed: `wad.rox` (Wad container, byte-list + from_bytes decode, positional map-lump
lookup — wadptr-safe), `mapdata.rox` (typed Linedef/Sidedef/Sector/Thing + vertex
parallel lists), `gfx.rox` (playpal/colormap/flats; picture-format patch decode with
slice-assign post blits; lazy TEXTURE1/2+PNAMES composition with clipped masked blits),
`viewer.rox`/`.qml` (palette/flats/textures/automap modes, keys 1-4 n/p). miniwad.wad
(229 KB, BSD, built from fragglet/miniwad via locally-extracted deutex + wadptr) checked
in with its COPYING; `get_freedoom.sh` downloads Freedoom 0.13.0 (viewer auto-prefers it).
Tests `doom_wad` + `doom_gfx` verified against independent Python parses; run under
runtests `--all` only (per David), with `-p examples/doom` injected for `doom_*`.

Freedoom-scale timings (Release, one-time costs): 28.8 MB load + 3163-lump dir 142 ms;
TexLib parse 90 ms; E1M1 full decode (1196 verts/1175 lines/182 sectors/292 things)
147 ms; texture compose ~1.4 ms lazy. No engine changes needed. Verified visually on
Freedoom: real flats/textures (incl. transparency masks) and the E1M1 automap.

Gotcha: a script's own `.roc` cache is not invalidated when an imported module changes —
use `--recompile` (roxal and runtests.py accept it) after editing modules.

- `wad.rox`: whole file → uint8 tensor; header + directory; name→lump dict (8-byte
  space-padded ASCII names). Verify the packed-byte-list → tensor construction is fast
  enough for freedoom1.wad (~20 MB); if not, a `read_file(format='tensor')` engine nicety.
- `gfx.rox`: PLAYPAL → `[256,3]`; COLORMAP → `[34,256]`; flats → `[64,64]`; the Doom
  picture format (patches: column posts) decoded to index tensor + 0/1 mask tensor;
  TEXTURE1/2 + PNAMES compositing patches into full texture tensors at load. Transparency
  composites as `dst*(1-mask) + src*mask` — no select op needed.
- Map lumps: VERTEXES, LINEDEFS, SIDEDEFS, SECTORS, SEGS, SSECTORS, NODES, THINGS.
- `viewer.rox`: palette/flat/patch browser + top-down automap of a map (Bresenham lines
  in Roxal; redraw-on-input only, so interpreted per-pixel is fine here).
- **Tests:** deterministic parse dump of miniwad (lump count, map counts, first vertex,
  texture names) vs `.out`. Wrinkle: tests live in `tests/` but `wad.rox` lives in
  `examples/doom/` — runtests.py needs to pass `--module-paths examples/doom` for these
  (small runtests.py extension, or the test does `import doom.wad` won't resolve — extend
  runtests).

### M3 — first-person walls: "walk around the map" (`render.rox`, `doom.rox`) — DONE 2026-07-18 (perf verdict: interpreter work now gates M4+)

Landed: SEGS/SSECTORS/NODES (+ node bboxes) in `mapdata.rox`; `render.rox` — front-to-back
BSP walk with R_CheckBBox subtree culling, faithful solidsegs
(R_ClipSolidWallSegment/R_ClipPassWallSegment transcription), per-column ceiling/floor
clip arrays, upper/lower/mid textures with correct pegging + rowoffsets, COLORMAP
distance lighting with fake contrast, perspective-correct 1/d + u/d interpolation;
`doom.rox` shell with spawn-thing start, line-collision movement (bbox-reject brute
force), floor-following viewz. Walks miniwad MAP01 and Freedoom E1M1 correctly
(verified on display: textures, pegging, portals, occlusion, collision). Test
`doom_render` (boolean invariants). Sky, flats, masked mids deferred to M4 as planned.

**Perf (Release, E1M1 spawn):** 273 segs processed, ~2.1k column draws/frame — healthy
Doom-like work counts — but 107 ms/frame (67 interp + 40 tensor); interactive 37–130 ms
by view (~8–27 fps). Microbenchmarks pin the cause: **for-loop iteration ~1.66 µs,
VM call ~1.4 µs, property access ~0.57 µs, list index ~0.5 µs** (Release). The column
loop is ~40 statements ≈ 25 µs/column — irreducible in the current interpreter.
**Per the M1 gate (>33 ms): the parked interpreter surgery (register VM / NaN-boxing)
is now the prerequisite for M4+.** Tensor-side fusion (~40 ms → ~20) is worthwhile but
secondary. Optimizations already applied: bbox culling, solidsegs, inlined column loop,
per-sidedef texture precompose, hot-array hoisting (property-access avoidance).

Renderer notes for the future: importers see module vars as import-time snapshots —
expose rebound state through accessor funcs (map(), stats(), ...); mid textures on
two-sided lines are masked draws → M4 with sprites.

Transcribe the wall pipeline: front-to-back BSP traversal with the solidsegs occlusion
list (`r_bsp.c`), per-seg column ranges, upper/lower/mid texture columns with pegging
(`r_segs.c`), colormap distance lighting, sky-flagged ceilings as sky columns. Player:
noclip movement first, then simplified `P_TryMove` line collision (blockmap optional —
brute-force line checks per subsector are fine at these map sizes).

**Acceptance:** walk through miniwad MAP01 and Freedoom E1M1 with correct wall geometry
and lighting at the M1-established frame rate. Handles both IWAD map namings (ExMy/MAPxx).

### M4 — flats + sprites: it looks like Doom — DONE 2026-07-18

Landed (commit 61fa9b3c): visplanes with pooled int32-tensor top/bottom arrays
(R_MakeSpans/R_MapPlane; spans as bulk ops), episode-keyed sky columns, drawsegs with
per-range opening captures, vissprites (things.rox type→sprite table, rotations/mirrors,
far→near with silhouette clipping), masked mid textures interleaved by depth. Verified on
E1M1: textured floors/ceilings with perspective, night-sky through skylights, pickups
visible through the see-through grate, control-room screens. E1M1 now ~500 ms/frame
(perf deliberately parked per David — interpreter surgery is the fix, not renderer work).

Roxal gotcha found here: `var t = obj.prop` where prop is a tensor takes a **COW value
copy** — writes to `t` don't reach `obj.prop`. Property-chain writes (`obj.prop[i] = v`)
and method calls (`obj.prop.fill(v)`) do mutate. Cost a debugging round; accessor lesson
from M3 plus this one belong in roxal-for-devs eventually.

- Visplanes (`r_plane.c`): floors/ceilings as horizontal span gathers —
  `flat.take((v*64 + u)` where u,v are per-span linear ramps → row slice-assign.
- Things → sprites: sprite lump lookup with rotations, distance sort, masked column
  draws clipped against drawsegs (the fiddliest transcription in the project).
- **Acceptance:** E1M1 with floors, ceilings, sky, and decorations at frame rate;
  screenshot-diff headless test.

### M5 — game: shoot a barrel, open a door — DONE 2026-07-18 (core loop)

Landed (commit 6df1e8e6): `gen_tables.py` → `tables.rox` (967 states, 137 mobj types,
P_Random table — generated offline from GPL info.c per plan, committed); `game.rox` mobj
state machine + AI (A_Look/A_Chase/A_*Attack, fireball missiles, pain/death/xdeath,
barrel A_Explode), pistol hitscan with autoaim, DR/D1 doors (verified open→wait→close
cycle on E1M1's first door: 124-unit travel, exact re-close); HUD status bar + digits +
first-person pistol with flash; 35 Hz fixed tics. Live-verified two-way combat on E1M1
(monsters wake/chase/attack, health/ammo track, player death).

Gameplay perf (commit 2bd06eec): BLOCKMAP collision (135× vs full scan) + tic cap +
per-tic solid_mobjs list — live E1M1 combat 3fps→14fps (the FPS readout exposed
~175ms/frame of tick cost, all collision scans, not render).

Still open for later polish: lifts/crushers/teleports, key-locked doors, switch texture
animation, level exit transitions, more weapons + weapon state machine, flat/wall
animations, sound (needs a qt audio path), savegames. Documented simplifications:
distance-based sight (LOS only gates hitscan attacks), direct-step chase instead of the
8-direction dance.

### The performance add-on — step 1 (VM profile) DONE 2026-07-18

Callgrind (instruction-exact; kernel perf is locked down on this box) on Release,
`s = s + 1` in a for-range loop: **8,896 instructions/iteration** across ~21 opcodes.
Breakdown and fix list, ranked:

1. ~~`VM::moduleType()` copied a full CallFrame per call (4 calls/iter — every
   module-var access)~~ — **FIXED** (30ab5f7d, one char): 8.3% of all instructions.
2. **Generic `indexValue` drives `for … in range` (15%)**: 1,354 instr/iter — full
   type dispatch + `ObjRange::length()` computed twice, every iteration. Fix: a
   dedicated iterate fast path (int counter in the frame) or a range fast path in
   the Index opcode.
3. **~40% of instructions sit at un-inlined call boundaries**: `Value::type()`,
   `asInt`, `Value` copy/dtor, `Thread::push/pop/peek` are cross-TU calls. Fix:
   `-flto` (or move hot methods into headers) — cheapest big multiplier to try.
4. **`VariablesMap::load/storeIfExists` take a `std::mutex` per module-var access**
   (2 locks/iter) plus a hash lookup. Fix: lock-free read path (or thread-confined
   module vars with an epoch check).
5. **Operator-hash dispatch attempted 3×/iter** before the int+int fast path
   (`tryDispatchBinaryOperator`). Fix: check the int/real fast path first.
6. **GC `isCollectionRequested()` is an out-of-line call once per opcode**
   (1.05M calls). Fix: inline it (atomic load) or poll on back-branches only.
7. `Value::tryResolveFuture` runs ~2×/iter on plain value reads.

Clean-machine baseline after fix 1: loop iter 890 ns, call 619 ns, property 218 ns,
list index 187 ns; E1M1 M5-renderer sweep 199 ms/frame. (Earlier 1.66 µs figures were
inflated ~2× by orphaned GUI-session processes — when benchmarking, `pgrep roxal`
first.)

**Step 2 (targeted fast paths) DONE 2026-07-19**, commit c32599e0: items 2, 5, 6 —
inline int/real fast paths in Add/Sub/Mul + all six comparisons (before the future
check / operator-hash / generic binaryOp), a range[int] fast path in Index (kills the
double length()), and the GC poll inlined. All suites + conversions pass on both
builds. Clean results: loop 890 → **540 ns/iter** (1.65×), E1M1 sweep 199 → **159 ms**,
raycaster 13.5 → **11.2 ms**.

**LTO experiment (build-lto/, CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON)**: loop unchanged
(540 ns) but real workloads gain: E1M1 159 → **145 ms**, raycaster 11.2 → **9.3 ms**
(~1.45× vs pre-perf-work on the raycaster). Worth adopting for release builds
(build time roughly doubles). The flat loop result is diagnostic: GCC declines to
inline even trivial Value helpers (asInt = a bit-extract) into the very large
VM::execute — function-growth limits, LTO or not. Post-fast-path profile is ~77%
execute + tiny helpers (asInt 8%, operator= 6%, type() 5%, dtor 5%, push/pop 6%).

**Steps a+b DONE 2026-07-19**, commit 14778912. The big find: `Value::val` is an
atomic whose inline ctors/copy/assign used default **seq_cst** — a full memory fence
on every Value construction/copy/assignment in the interpreter. Stores are now
release, loads relaxed (plain movs on x86; publication is externally synchronized by
locks/safepoints). Plus: VariablesMap mutexes → SpinLock (same critical sections),
Thread::push/pop/popN/peek inlined, asIntUnchecked/asRealUnchecked in the guarded
fast paths. All suites green both build types.

| bench (Release, idle) | pre-perf | after all fixes | + LTO |
|---|---|---|---|
| loop iter | 890 ns | **334 ns** | 369 ns |
| raycaster frame | 13.5 ms | 9.8 ms | **7.7 ms** |
| E1M1 render sweep | 199 ms | 121 ms | **108.7 ms** |
| Doom gameplay (252 mobjs) | ~500-670 ms | — | **~95 ms (~10 fps)** |

(Note for LTO dirs: configure with -DROXAL_ENABLE_QT=ON
-DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64 or the qt plugin is silently absent —
and .roc caches are not portable between differently-built binaries; --recompile
when switching.)

Loop instructions: 8.9k → 6.5k/iter (wall 2.7× — the fences showed in cycles, not Ir).

**Remaining levers, ranked**: (c) the residue is now truly structural — execute() is
55% self (dispatch switch + opcode bodies) and GCC still outlines Value copy/assign/
dtor + VM::push/pop wrappers into it (growth limits): computed-goto dispatch /
superinstructions / forced-inline sweep is the next conversation, needs David's
architectural blessing; (d) tensor-op fusion — design in tensor-fusion-design.md; **option 1
(temporary reuse) DONE 2026-07-19, commit bc183bb3**: dead-temporary
tensor(+)scalar ops mutate in place (chain microbench 10 -> 7 us; raycaster
9.8 -> 8.7 ms; E1M1 121 -> 114 ms). Gotcha: ObjControl weak baseline is 1,
not 0 (strong refs collectively hold one weak). **Contiguous-slice fast path
DONE 2026-07-19, commit 1eff0fbe** (rectangular block copy, memcpy per
innermost run: chain 7.0 -> 3.9 us, raycaster 7.4 / 6.6 LTO ms, E1M1
107 / 103 LTO ms; FPS print added f18fc3e4). take/astype allocations are
what remain of the tensor share — that's option 2 (blit_col/blit_span),
still drafted, David judged it more Doom-specific; (e) adopt CMAKE_INTERPROCEDURAL_OPTIMIZATION for
release builds (build-lto/ has it; ~2× build time, ~10% runtime). Doom gameplay ≈
4-5 fps now; 30 fps needs ~4× more, split between (c) and (d).

- Movement feel (momentum, friction, step-up), use-lines (doors), lifts, switches.
- Hitscan pistol, damage, exploding barrels.
- HUD status bar + WAD font.
- Monsters: transcribe the `states`/`mobjinfo` tables — mechanical but huge; generate
  `tables.rox` offline from `info.c` with a small Python script (committed output;
  runtime stays pure Roxal), then the A_Look/A_Chase/A_*Attack subset.

### Non-goals (initially)

Demo playback, netplay, savegames, menus, music/SFX (qt has no audio path today — adding
QSoundEffect to the qt module later is platform work, not game work), Boom extensions.

## Assets

- **Check in `examples/doom/miniwad.wad`** (Fraggle's minimalist IWAD, <0.25 MB, built
  from Freedoom assets, BSD) — development + CI target; exercises every lump type.
- **`get_freedoom.sh`** downloads freedoom1.wad (~20 MB, BSD) for the real demo.
- README documents both licenses + the GPL2 lineage of transcribed renderer logic.

## File layout

```
examples/doom/
  PLAN.md  README.md
  raycaster.rox raycaster.qml     # M1 (kept as standalone perf benchmark)
  doom.rox doom.qml               # main entry
  wad.rox gfx.rox render.rox game.rox tables.rox viewer.rox bench.rox
  miniwad.wad  get_freedoom.sh
```

## Testing strategy

- Pure-logic tests (WAD parsing, geometry decode) in `tests/` with `.out` files.
- Headless render tests: software backend + `qt.post_key` scripted input +
  `engine.grab_window()` pixel assertions.
- `bench.rox` prints frame-time percentiles; only meaningful from `build-rel/`.

## Correctness pass vs Chocolate Doom (2026-07-21, branch doomopt)

Verified the renderer against chocolate-doom 3.0 running the same freedoom1
E1M1 views (a generated PWAD relocates the player start to arbitrary coords —
see the session's start_*.wad trick; choco lives in ~/chocolate-doom-local/).
Findings, all fixed:

1. **Visplane coverage bug** (the "brown flat floor patches"): draw_range
   closed ceilclip/floorclip unconditionally when a two-sided seg had no
   upper/lower texture; vanilla r_segs.c gates that on markceiling/markfloor.
   Everything behind same-height portals (floors, sky, whole subtrees via
   bbox culling) could go unpainted, showing the frame-clear color. Also
   adopted the real sky hack (worldtop = backtop for sky-to-sky portals, keep
   marking) and texture-gated step clipping. `snap.rox` leak scan (292
   positions x 4 angles, double-render background diff): 554/1168 leaking
   views before, 0 after. Costs 4-8% frame time in extra (correct) spans.
2. **2D monster sight** (the "shot by nothing" / standing-still death):
   monsters now use a 3D P_CheckSight-lite (`sight_clear` in doom.rox) — a
   slope window narrowed by each two-sided opening along the blockmap-marched
   sight line. Window sills/ledges/closed doors block sight like vanilla;
   standing at the E1M1 spawn now takes zero damage (was dead in ~38 s).
3. **Feedback + fairness**: vanilla red damage palette tint (PLAYPAL rows,
   ST_doPaletteStuff formula) via render.set_palette; hitscan hit chance now
   falls off with distance (~vanilla spread); monsters only spot the player
   in their forward 180° arc (P_LookForPlayers rule); window is 4:3.
4. **wall_dist blockmap march**: sight/hitscan rays walk BLOCKMAP cells
   (P_PathTraverse style) instead of scanning all 1175 lines: 4.7 ms → 0.09 ms
   per ray, identical over 2336 test rays.

Diagnostics kept: snap.rox (coverage scan), views.rox (fixed viewpoints),
combat.rox / walksim.rox (damage-source visibility), rbench.rox (render
timings). Engine issues found on the way (not doom bugs): fileio async
writes can be dropped at process exit unless something forces a flush
(read_file after close works around it); build-lto has ROXAL_ENABLE_MEDIA
off, so media.Image methods silently run their empty stub bodies.

## Risks

1. **Kernel-dispatch overhead on ~200-element tensors** — 320 columns × ~5 bulk ops/frame.
   M1 exists to measure exactly this before committing to Doom's renderer.
2. **GC pressure** from per-frame temporaries (~50k small tensors/s at 30 fps) —
   watch GC pauses in M1; contingency is in-place/`out=` variants (M0 note).
3. **Sprite clipping vs drawsegs (M4)** — the classically fiddly part; Managed Doom's
   version is the cleanest reference.
4. **Freedoom maps target Boom compatibility** — irrelevant to lump formats/rendering;
   a few gameplay specials may be Boom-generalized (ignore unknown specials gracefully).
