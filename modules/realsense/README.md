# realsense — Intel RealSense depth cameras for Roxal, via FFI

A pure-Roxal binding: `init.rox` declares the API with `@cfunc` over a small
flat-C shim (`librsshim.so`). The roxal binary has **no** librealsense
dependency — the shim is `dlopen`ed by `sys.loadlib()` on `import realsense`.

**User-facing API documentation: [realsense-guide.md](realsense-guide.md).**
This file covers architecture, building, and extending.

## Why a shim (and not `@cfunc` straight onto `rs2_*`)

librealsense already exposes a flat C API, so binding it directly is tempting.
Two things get in the way:

- **Every** `rs2_*` call reports failure through a trailing `rs2_error**`
  out-parameter. Roxal's FFI writes back through pointers to *primitives*, but
  has no way to receive an opaque handle written through a `T**`, so the error
  channel cannot be expressed.
- Frames, queues, processing blocks and stream profiles have lifetime rules
  (`rs2_process_frame` consumes its input; profile pointers belong to their
  list) that are easy to get wrong from script code.

The shim absorbs both: it returns plain scalars and NUL-terminated strings,
reports errors via `rs_last_error()`, and owns every librealsense object.

## Conventions

- **Depth** is a uint16 tensor `[height, width, 1]` in raw sensor units;
  multiply by `depth_scale` (or call `depth_meters`) for metres. 0 means "no
  reading at this pixel".
- **Colour** is a uint8 tensor `[height, width, 3]` in RGB order — the same
  convention as the opencv and media modules. The camera is configured for
  `RS2_FORMAT_RGB8` directly, so no channel swap happens anywhere.
- Calibration (intrinsics, extrinsics, depth scale) is **copied out at open
  time**: the `rs2_stream_profile` pointers it comes from belong to the profile
  list and must not outlive it.
- `close()` releases the device now; the handle is freed later by the GC
  finalizer. Both are safe to call, in either order.
- Errors raise `RuntimeException` with librealsense's message. A `read()`
  timeout is *not* an error — it returns `nil`.

## Layout

- `init.rox` — the module (hand-written; enum values mirror librealsense's own
  headers).
- `shim/rs_shim.cpp` — the flat-C shim.
- `shim/build.sh` — builds `librsshim.so` against `deps/librealsense/install`
  with an `$ORIGIN`-relative rpath (no environment setup needed at runtime).

## Building

`./install-deps.sh librealsense` from the repo root does everything: it builds
librealsense 2.58.3 into `deps/librealsense` and then runs `shim/build.sh`.
It needs the `libudev-dev` and `libusb-1.0-0-dev` apt packages (the script
installs them).

`BUILD_WITH_DDS=ON` is set so Ethernet-attached cameras (D555 and friends)
enumerate alongside USB ones; that is what makes the dep build slow, since it
also fetches Fast-DDS. USB-only setups do not need it.

For non-root access to the IMU and some device metadata, install the SDK's
udev rules once:

```
sudo cp deps/librealsense/config/99-realsense-libusb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

`tests/realsense_basic.rox` runs when `librsshim.so` exists *and* a camera is
plugged in; `runtests.py` skips it otherwise.

## Two things worth knowing about the SDK's C API

Both cost real debugging time, so they are called out here:

- `rs2_get_frame_texture_coordinates` is declared as returning `rs2_pixel*`
  (`int[2]`), but the buffer actually holds `{float u, v}` pairs normalised to
  `[0, 1]` — the header's own prose says so, and the C++ wrapper casts the
  result to its float-based `texture_coordinate`. Reading it as ints is silent
  nonsense.
- `rs2_align` refreshes only the streams it aligns. Its output frameset carries
  a stale infrared frame — 19 of 20 reads came back bit-identical, against 0 of
  20 with alignment off — so an IR view built from the aligned set looks frozen
  while depth and colour update. `rs_read` therefore copies infrared out of the
  frameset *before* handing it to the aligner (which also has to happen first,
  since `rs2_process_frame` consumes the caller's reference).
- Texturing a point cloud is not just "process the colour frame": the block
  ignores any frame that does not match its `STREAM_FILTER`,
  `STREAM_FORMAT_FILTER` and `STREAM_INDEX_FILTER` options, which is what the
  C++ `pointcloud::map_to()` sets before processing. Miss them and the uv map
  comes back all zeros, with no error.

## Extending

The shim is deliberately small. Natural next steps, roughly in order of value:

- **Decimation filter** — the one post-processing block still missing, because
  it changes the depth resolution: it needs the cached frame sizes and the
  aligner's geometry renegotiated after the first filtered frame.
- **Multi-camera sync** — `RS2_OPTION_INTER_CAM_SYNC_MODE` plus opening several
  sessions by serial.
- **Recording and playback** — `rs2_create_record_device` / rosbag playback,
  which would let vision code run against captured scenes with no hardware.
- **Advanced-mode presets** — `rs_advanced_mode.h` JSON blobs, for loading
  Intel's tuned depth configurations wholesale.
