# The Roxal `realsense` Module

Intel RealSense depth cameras for Roxal, implemented as **pure Roxal over FFI** —
`init.rox` declares everything with `@cfunc` against a small flat-C shim
(`librsshim.so`); the roxal binary itself has no librealsense dependency.

```roxal
import realsense.*

var cam = open(width=640, height=480, fps=30, align=true)
var f = cam.read()                            # depth + colour, hardware-synced
var metres = cam.depth_meters(f)              # float32 [H, W, 1]
cam.close()
```

## What this gives you that a plain camera capture does not

A RealSense streams over UVC, so `opencv.open_camera` can read its colour image
— but not its depth, and none of its calibration. This module carries the parts
that make depth *metric and registered*: the factory intrinsics and extrinsics,
the depth scale, hardware depth-to-colour alignment, matched framesets, and the
sensor controls (emitter power, exposure, presets).

## Conventions

- **Depth is a uint16 tensor `[height, width, 1]` in raw sensor units.**
  Multiply by `cam.depth_scale` (1 mm per unit on D4xx) or call `depth_meters`.
  A 0 means the sensor got no return at that pixel — not "zero distance".
- **Colour is a uint8 tensor `[height, width, 3]` in RGB order**, the same
  convention as the `opencv` and `media` modules, so frames flow between them
  without conversion.
- **Errors raise `RuntimeException`** carrying librealsense's message. A read
  timeout is not an error: `read()` returns `nil`.
- **Handles are garbage collected.** `close()` releases the device immediately
  (emitter off, free for other processes); the GC would eventually do it.

## Finding cameras

```roxal
for d in devices():
  print("[{d.index}] {d.name}")     # serial, firmware, usb, product_line too
```

`Device` has `index`, `name`, `serial`, `firmware`, `usb`, `product_line`.
Fields a model does not report come back as `''`.

`reset_device(index)` hardware-resets a camera: it drops off the bus and
re-enumerates a second or two later. That is the cure for a camera that has
wedged mid-stream (USB `-71` protocol errors in `dmesg`, reads timing out),
which repeated open/close cycles can provoke.

## Opening a camera

```roxal
var cam = open(serial='', depth=true, color=true, ir=false, imu=false,
               width=0, height=0, fps=0, align=false)
```

- `serial` — pick a specific device (`''` = first found). Prefer this over an
  index for anything long-lived: it is stable across replugs, and network
  cameras have no meaningful index.
- `ir` — also stream the left infrared imager (uint8 `[H, W, 1]`), which shares
  the depth camera's viewpoint.
- `imu` — also stream the gyroscope and accelerometer, on models that have them
  (D435i, D455). This needs the SDK's udev rules installed — see the README —
  otherwise opening fails with a permission error on the IIO sysfs nodes.
- `width`/`height`/`fps` — requested mode for both streams; 0 asks for the SDK
  default. The nearest supported mode is chosen, so read back what you got from
  `cam.depth_width`, `cam.color_height`, and so on.
- `align` — reproject depth onto the colour image so `depth[y, x]` and
  `color[y, x]` are the same point in the scene. Requires both streams. Aligned
  depth takes the colour stream's geometry *and* its intrinsics.

`Camera` exposes `depth_width`, `depth_height`, `color_width`, `color_height`
and `depth_scale`.

> **USB 3 matters.** A D435i on a USB 2 cable enumerates fine and streams
> colour, but depth delivery collapses to a frame every few seconds. If depth
> reads keep timing out, check `usb` on the `Device` (or `lsusb -t`) before
> anything else — a cable marked SS is the fix.

## Reading frames

```roxal
var f = cam.read(timeout_ms=5000)   # Frameset, or nil if none arrived in time
f.depth                             # uint16 [H, W, 1], raw units (nil if disabled)
f.color                             # uint8  [H, W, 3] RGB (nil if disabled)
f.ir                                # uint8  [H, W, 1] left infrared (nil if disabled)
f.gyro                              # float64 [3] rad/s (nil if disabled)
f.accel                             # float64 [3] m/s^2, gravity included
f.timestamp                         # device timestamp, milliseconds
```

The IMU samples much faster than the video streams, so `gyro`/`accel` carry the
most recent sample rather than one reading per frame.

Depth and colour in one `Frameset` come from the same hardware-synchronised
set — that is the reason to take both from one camera object rather than two
independent captures.

`cam.depth_meters(f)` converts a frameset's depth to float32 `[H, W, 1]` in
metres (the conversion runs as a typed loop in the shim, not per-pixel Roxal).

## Calibration

```roxal
var intr = cam.intrinsics(Stream.Depth)    # or Stream.Color
print("{intr.fx} {intr.fy} {intr.ppx} {intr.ppy}")
```

`Intrinsics` has `width`, `height`, `ppx`, `ppy`, `fx`, `fy`, `model` (a
`Distortion` value) and `coeffs` (float64 `[5]`).

`cam.extrinsics()` returns the rigid transform from the depth frame to the
colour frame as `Extrinsics{rotation: float64 [3, 3], translation: float64 [3]}`
in metres. (With `align=true` you rarely need it — alignment has already put
depth in the colour frame.)

```roxal
var p = cam.point_at(f, x, y)          # float64 [3] metres, or nil if no depth
```

`point_at` deprojects a pixel to a 3D point in the camera frame: +x right,
+y down, +z forward. The maths is librealsense's own `rs2_deproject`, so the
stream's distortion model is applied exactly rather than approximated.

## Point clouds

```roxal
var f = cam.read()
var pc = cam.points()          # for the frameset just read
pc.xyz                         # float32 [N, 3] metres, one point per depth pixel
pc.uv                          # float32 [N, 2] texture coords in [0, 1], or nil
```

`points()` works from the frames the last `read()` retained, so the cloud always
matches the frameset in hand. With the colour stream enabled the cloud is
textured against it: multiply `uv` by the colour image's width and height for
pixel coordinates, and treat values outside `[0, 1]` as points the colour camera
cannot see. Points with no depth reading come back as `(0, 0, 0)`.

## Depth post-processing

```roxal
cam.enable_filter(Filter.Spatial)
cam.set_filter_option(Filter.Spatial, FilterOption.Magnitude, 3.0)
cam.enable_filter(Filter.HoleFilling)
var f = cam.read()             # filters applied, before alignment
cam.enable_filter(Filter.Spatial, false)
```

`Filter.Spatial`, `Filter.Temporal` and `Filter.HoleFilling` run in that order,
on the whole frameset (non-depth streams pass through untouched), before
alignment. They are off by default, and they meaningfully improve depth
coverage — on a typical desk scene, enabling spatial + hole-filling took valid
depth pixels from ~269k to ~305k of 307200.

Tune them with `set_filter_option`/`get_filter_option` and `FilterOption`:
`Magnitude` (spatial passes, 1–5), `SmoothAlpha`/`SmoothDelta` (spatial and
temporal edge preservation), `HolesFill` (fill aggressiveness).

Decimation is deliberately not exposed: it changes the depth resolution, which
would invalidate every cached frame size and the aligner's geometry.

## Sensor options

```roxal
if cam.supports_option(Sensor.Depth, Option.LaserPower):
  var r = cam.option_range(Sensor.Depth, Option.LaserPower)
  cam.set_option(Sensor.Depth, Option.LaserPower, r.max)   # brightest emitter
  var now = cam.get_option(Sensor.Depth, Option.LaserPower)
  print("laser power now {now}")
```

`Sensor.Depth` / `Sensor.Color` select which sensor; `supports_option` tells you
what this particular model has, and `option_range` returns
`OptionRange{min, max, step, default_value}`.

`Option` covers what a D400-series camera honours: `Exposure`, `Gain`,
`WhiteBalance`, `AutoExposure`, `AutoWhiteBalance`, `VisualPreset`,
`LaserPower`, `ConfidenceThreshold`, `EmitterEnabled`, `FramesQueueSize`,
`DepthUnits`, `GlobalTimeEnabled`. Values match librealsense's own enum, so an
option it lacks a name for can still be passed as an integer.

## Combining with opencv

Colour frames need no conversion, and depth-derived images can be treated as
single-channel data:

```roxal
import opencv
import realsense

var cam = realsense.open(width=640, height=480, fps=30, align=true)
var f = cam.read()
var edges = opencv.canny(opencv.grayscale(f.color), 50.0, 150.0)
```
