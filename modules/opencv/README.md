# opencv — OpenCV 5 for Roxal, via FFI

A pure-Roxal binding: `init.rox` declares the API with `@cfunc` over a small
flat-C shim (`libcvxshim.so`). The roxal binary has **no** OpenCV dependency —
the shim is `dlopen`ed by `sys.loadlib()` on `import opencv`.

**User-facing API documentation: [opencv-guide.md](opencv-guide.md).** This
file covers architecture, building, and extending.

## Conventions

- Images are **uint8 tensors of shape [height, width, channels] in RGB order**
  (the same convention as `media.Image` and `qt.FrameView`). The shim converts
  to/from OpenCV's native BGR at the boundary.
- Same-size operations run zero-copy: the shim wraps tensor buffers as borrowed
  `cv::Mat` headers. Handles (decoded images, `VideoCapture`, contours) are
  opaque `foreignptr`s freed automatically by GC finalizers (`@cfunc free=`).
- Errors raise `RuntimeException` with OpenCV's message.

## Layout

- `init.rox` — the module. Hand-written API plus a generated section (enums,
  same-shape filter family) between the `BEGIN/END GENERATED` markers.
- `shim/cvx_shim.cpp` — hand-written shim core (+ `#include "cvx_gen.inc"`).
- `shim/cvx_gen.inc` — generated C shim functions.
- `shim/generate.py` — regenerates both generated parts from OpenCV's own
  binding metadata (`hdr_parser.py` in the OpenCV source). Enum values are
  read from the headers, never transcribed.
- `shim/build.sh` — builds `libcvxshim.so` against `deps/opencv/install`
  with an `$ORIGIN`-relative rpath (no environment setup needed at runtime).

## Building

1. Build OpenCV 5 once: clone tag `5.0.0` to `deps/opencv`, then
   `cmake -B build -S . -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
   -DCMAKE_INSTALL_PREFIX=$PWD/install ...` and `cmake --install build`
   (FFmpeg dev packages enable video-file support in `videoio`).
2. `./shim/build.sh` → `libcvxshim.so` appears next to `init.rox`.
3. Optionally regenerate first: `python3 shim/generate.py && ./shim/build.sh`.

Tests (`tests/opencv_*.rox`) run automatically when `libcvxshim.so` exists;
`runtests.py` skips them otherwise.

## Extending

For a new same-shape filter, add a row to `FILTERS` in `shim/generate.py`.
For a new enum group, add a row to `ENUM_SPECS`. Anything with an irregular
shape contract (output size differs, variable-length results, stateful
objects) gets a hand-written shim function in `cvx_shim.cpp` and a wrapper in
`init.rox` — see contours or `VideoWriter` for the pattern.
