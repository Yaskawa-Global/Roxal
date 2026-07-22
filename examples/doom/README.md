# Doom in Roxal

A work-in-progress port of Doom written in pure Roxal (no native game module),
rendered through the qt module's software FrameView framebuffer. The
implementation plan and milestone status live in `PLAN.md`.

## Contents

| File | What it is |
|---|---|
| `raycaster.rox` | M1: Wolfenstein-style textured raycaster (interactive; WASD/arrows, Esc) |
| `raycaster_smoke.rox` | headless checks for the raycaster (`QT_QPA_PLATFORM=offscreen`) |
| `raycaster_bench.rox` | raycaster frame-time benchmark (run with the Release build) |
| `raycast_core.rox` | shared raycaster core: map, textures, DDA renderer, movement |
| `wad.rox` | M2: WAD container — directory parsing, lump lookup, field decoding |
| `mapdata.rox` | map geometry decode (vertexes, linedefs, sidedefs, sectors, things) |
| `gfx.rox` | palette, colormap, flats, picture-format patches, texture composition |
| `sound.rox` | DS* DMX sound lumps → media.Audio clips; positional volume/pan, per-tic dedupe |
| `viewer.rox` | WAD asset viewer: palette / flats / textures / automap (keys 1-4, n/p) |
| `miniwad.wad` | tiny BSD-licensed IWAD used for development and tests |
| `get_freedoom.sh` | downloads the full Freedoom IWADs (viewer prefers freedoom1.wad) |

Run everything from the repository root with the Release build, e.g.:

```
./build-rel/roxal examples/doom/viewer.rox
```

Tests (`tests/doom_*.rox`) run via `python3 runtests.py --all -t 'doom_*'`.

## Asset licenses

- `miniwad.wad` is built from [fragglet/miniwad](https://github.com/fragglet/miniwad),
  a minimalist Doom II-compatible IWAD derived from Freedoom assets. BSD
  3-clause, © contributors to the Freedoom project — see `miniwad-COPYING.adoc`.
- [Freedoom](https://freedoom.github.io/) (downloaded, not checked in) is BSD
  3-clause under the same copyright.
- Renderer logic transcribed from the GPL2 linuxdoom/Chocolate Doom sources
  makes the Roxal game code GPL2-compatible.
