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
| `encode_capture.sh` | turns a `doom.rox --capture` dump into an MP4 (see below) |

Run everything from the repository root with the Release build, e.g.:

```
./build-rel/roxal examples/doom/viewer.rox
```

Tests (`tests/doom_*.rox`) run via `python3 runtests.py --all -t 'doom_*'`.

## Recording a clip

A screen recorder captures the frame rate the renderer actually managed, which
varies with what is on screen. `doom.rox --capture` records the game instead:
every frame advances exactly one 35 Hz tic and is appended to the file as raw
320x200 rgb24, so render speed decides only how long the session takes, never
what the video looks like.

```
./build-rel/roxal examples/doom/doom.rox --capture frames.raw
examples/doom/encode_capture.sh frames.raw doom.mp4
```

The encoder upscales with nearest-neighbour to 4:3 (`-s 4` → 1280x960, matching
the window; the framebuffer's pixels are not square) and writes a constant-rate
H.264 MP4. It defaults to 30 fps — one frame per captured tic, so motion runs
17% slow, in exchange for a rate nothing will re-encode; `-f 35` plays at true
speed. `-a` adds a silent audio track for players that expect one.

Capturing costs about 6.4 MB per second of video, and play runs in slow motion
while it is on: each frame is 1/35 s of game time however long it took to draw.
Sound keeps running in real time and is not captured.

## Asset licenses

- `miniwad.wad` is built from [fragglet/miniwad](https://github.com/fragglet/miniwad),
  a minimalist Doom II-compatible IWAD derived from Freedoom assets. BSD
  3-clause, © contributors to the Freedoom project — see `miniwad-COPYING.adoc`.
- [Freedoom](https://freedoom.github.io/) (downloaded, not checked in) is BSD
  3-clause under the same copyright.
- Renderer logic transcribed from the GPL2 linuxdoom/Chocolate Doom sources
  makes the Roxal game code GPL2-compatible.
