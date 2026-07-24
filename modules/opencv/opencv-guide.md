# The Roxal `opencv` Module

OpenCV 5 computer vision for Roxal, implemented as **pure Roxal over FFI** —
`init.rox` declares everything with `@cfunc` against a small flat-C shim
(`libcvxshim.so`); the roxal binary itself has no OpenCV dependency.

```roxal
import opencv.*

var img = imread('photo.jpg')                 # uint8 [H, W, 3] RGB tensor
var edges = canny(gaussian_blur(grayscale(img), 5, 1.5), 50.0, 150.0)
imwrite('edges.png', edges)
```

See `examples/opencv-blobs.rox` (image pipeline) and
`examples/opencv-camera.rox` (live camera + ArUco + FrameView display).

## Conventions

- **Images are uint8 tensors of shape `[height, width, channels]` in RGB
  order** — the same convention as `media.Image` and `qt.FrameView`, so tensors
  flow between all three without conversion. Grayscale images are `[H, W, 1]`.
  OpenCV's native BGR order never leaks through the API.
- **Errors raise `RuntimeException`** carrying OpenCV's message.
- **Handles are garbage collected.** Decoded images, detections, capture and
  writer objects free their native resources automatically (via `@cfunc free=`
  finalizers). The exceptions that want an explicit call: `VideoWriter.close()`
  (flushes the file — GC is not prompt enough) and optionally
  `VideoCapture.close()` (releases the camera immediately).
- Same-size operations are **zero-copy**: the shim wraps tensor buffers as
  borrowed `cv::Mat` headers. In-place drawing follows tensor copy-on-write
  semantics exactly like `t[y, x, c] = v`.
- Matrix-valued data (camera matrices, transforms) are **float64 tensors**;
  point sets are **float32 tensors** (`[N, 2]` image points, `[N, 3]` object
  points); ids/indices are **int32 tensors**.

## Images and codecs

| Function | Notes |
|---|---|
| `imread(path, channels=3) -> tensor` | decode a file; `channels`: 3 RGB, 1 gray, 4 RGBA |
| `imwrite(path, img)` | format chosen by extension |
| `imencode(img, ext='.png', quality=95) -> tensor` | encode to in-memory bytes (`[N]` uint8) — e.g. to publish frames over DDS |
| `imdecode(data, channels=3) -> tensor` | decode in-memory bytes |

## Image processing

All of these return a new image; the input is untouched.

- `resize(img, width, height, interpolation=Interpolation.Linear)`
- `cvt_color(img, code)` — RGB-centric `ColorCode` values; `grayscale(img)` is
  the common shortcut (RGB → `[H, W, 1]`)
- Blurs: `gaussian_blur(img, ksize, sigma=0.0)`, `median_blur(img, ksize)`,
  `blur(img, ksize)`, `bilateral_filter(img, diameter, sigma_color, sigma_space)`
- Morphology: `erode(img, ksize, iterations=1, kshape=MorphShape.Rect)`,
  `dilate(...)`, `morphology(img, op, ksize, ...)` with `MorphOp.Open/Close/...`
- Edges & intensity (grayscale input): `canny(img, t1, t2)`,
  `sobel(img, dx, dy, ksize=3)`, `laplacian(img, ksize=3)`,
  `threshold(img, thresh, maxval=255.0, mode=ThresholdType.Binary)`,
  `threshold_otsu(img, maxval=255.0)`, `equalize_hist(img)`
- Geometry: `flip(img, axis)` (0 vertical, 1 horizontal, -1 both),
  `rotate(img, Rotation.Clockwise90/Half/CounterClockwise90)`,
  `warp_affine(img, m, width, height)` (`m` = float64 `[2, 3]`),
  `warp_perspective(img, m, width, height)` (`[3, 3]`),
  `rotation_matrix(cx, cy, angle_degrees, scale=1.0) -> [2, 3]`

## Drawing (in place, mutates the image)

Color is `[r, g, b]`; `thickness=-1` fills closed shapes; text/shapes are
anti-aliased.

- `line(img, x1, y1, x2, y2, color, thickness=1)`
- `rectangle(img, x, y, width, height, color, thickness=1)`
- `circle(img, cx, cy, radius, color, thickness=1)`
- `put_text(img, text, x, y, color, scale=1.0, font=Font.Simplex, thickness=1)`

## Contours

On a binary grayscale image (from `threshold`/`canny`):

```roxal
var blobs = find_contours(mask)               # list of int32 [N, 2] point tensors
for pts in blobs:
  var r = bounding_rect(pts)                  # [x, y, w, h]
  print(contour_area(pts))
```

`find_contours(img, mode=ContourMode.External, method=ContourApprox.Simple)`.

Analysis kit: `draw_contours(img, contours, color, thickness=2)` (in place,
-1 fills), `convex_hull(pts) -> [M, 2]`, `approx_poly(pts, epsilon,
closed=true) -> [M, 2]` (Douglas-Peucker simplification), `min_area_rect(pts)
-> RotatedRect{center_x, center_y, width, height, angle}` with
`box_points(rect) -> [4, 2]` corners for drawing.

## Segmentation and motion

```roxal
var hsv = cvt_color(frame, ColorCode.Rgb2Hsv)
var mask = in_range(hsv, [10, 100, 100], [25, 255, 255])   # orange things
for blob in connected_components(mask, min_area=100):       # list of Blob
  print("blob at {blob.center_x}, {blob.center_y}: {blob.area} px")
```

`in_range(img, lower, upper)` masks pixels whose channels all lie inside the
per-channel bounds — with an HSV conversion it is the classic color tracker.
`connected_components(mask, min_area=1)` labels the blobs and returns
`Blob{x, y, width, height, area, center_x, center_y}` (background excluded).

Motion against a static camera:

```roxal
var bs = BackgroundSubtractor()          # MOG2; history=500, threshold=16.0
var fg = bs.apply(frame)                 # [H, W, 1]: 255 fg, 127 shadow
```

Template matching (find a known patch): `find_template(img, templ,
method=TemplateMethod.CcoeffNormed) -> TemplateMatch{x, y, score}`.

Optical flow (grayscale inputs): `good_features(gray, max_corners=100, ...)`
seeds `flow_points(prev, next, points) -> FlowResult{points, status}`
(Lucas-Kanade), and `flow_dense(prev, next) -> [H, W, 2]` gives per-pixel
(dx, dy) (Farneback). `clahe(gray, clip_limit=2.0, tiles=8)` is
contrast-limited adaptive histogram equalization for poor lighting.

## Video

```roxal
var cap = open_camera(0)                      # or open_video('file.mp4')
var frame = cap.read()                        # RGB tensor; nil at end of stream
print("{cap.width} x {cap.height} @ {cap.fps()}")
cap.set(CapProp.PosFrames, 0.0)               # seek (files)
cap.close()                                   # release the device now

var w = VideoWriter('out.mp4', 30.0, 640, 480)   # codec='mp4v' default
w.write(frame)
w.close()                                     # REQUIRED: flushes the file
```

Camera capture uses V4L2; video files use FFmpeg (enabled when the OpenCV
build found the FFmpeg dev packages).

## ArUco markers

```roxal
var tag = generate_marker(ArucoDict.Dict4x4_50, 7, side=200)   # print me
var markers = detect_markers(frame, ArucoDict.Dict4x4_50)      # list of Marker
for m in markers:
  draw_marker(frame, m)                        # outline + id, in place
  var pose = marker_pose(m, 0.05, calib.camera_matrix, calib.dist_coeffs)
  print("id {m.id} at z = {pose.tvec[2]} m")
```

`Marker` has `id` and `corners` (float32 `[4, 2]`, order TL TR BR BL).
`marker_pose(marker, marker_size, camera_matrix, dist_coeffs=nil) -> Pose`
uses the printed side length; `tvec` comes back in the same unit.

## QR codes

`generate_qr(text, side=200)` renders a code; `detect_qr(img)` returns a list
of `QrCode{text, corners}` (decodes multiple codes per image).

## Calibration and pose

`Pose` holds `rvec` (Rodrigues rotation, float64 `[3]`) and `tvec` (`[3]`).

- `solve_pnp(object_points, image_points, camera_matrix, dist_coeffs=nil) -> Pose`
- `project_points(object_points, pose, camera_matrix, dist_coeffs=nil) -> [N, 2]`
- `find_chessboard(img, cols, rows) -> [cols*rows, 2] | nil` — inner corners,
  subpixel-refined
- `calibrate_camera(object_points_list, image_points_list, width, height) ->
  Calibration{camera_matrix [3,3], dist_coeffs [5], error}`
- `undistort(img, camera_matrix, dist_coeffs) -> tensor`

ChArUco boards are the better calibration target (subpixel corners with marker
identity, robust to partial views):

```roxal
var board = charuco_board(5, 4, 0.04, 0.025)        # squares, sizes in meters
imwrite('board.png', cvt_color(board_image(board, 1000, 800, 40), ColorCode.Gray2Rgb))

var objs = []
var imgs = []
for shot in shots:                                   # captured board photos
  var det = detect_charuco(shot, board)              # nil when not seen
  if det != nil:
    objs.append(det.object_points)                   # pre-matched [N,3] / [N,2]
    imgs.append(det.corners)
var calib = calibrate_camera(objs, imgs, 1920, 1080)
```

**Hand-eye calibration** (camera mounted on a robot gripper): collect the
gripper→base transform (from the robot) and the target→camera transform (e.g.
a ChArUco pose via `solve_pnp`) for several distinct orientations, then

```roxal
var cam2gripper = hand_eye(gripper_to_base, target_to_cam)   # [4, 4] float64
```

Both arguments are lists of float64 `[4, 4]` transforms; `method` defaults to
`HandEyeMethod.Tsai`.

Wide-FOV lenses: `calibrate_camera_fisheye(objs, imgs, w, h)` (fisheye
model, 4 distortion coefficients), `undistort_fisheye(img, camera_matrix,
dist_coeffs)`, and `project_points_fisheye(points, pose, camera_matrix,
dist_coeffs)`.

## Stereo

- `stereo_calibrate(object_points, left_points, right_points, leftCalib,
  rightCalib, width, height) -> StereoCalibration{rotation, translation, error}`
  — the fixed relative pose of a rigid camera pair (intrinsics held fixed)
- `stereo_rectify(leftCalib, rightCalib, stereoCalib, width, height) ->
  StereoRectification` — row-aligning transforms (`left_/right_rotation`,
  `left_/right_projection`, `q`)
- `rectify(img, calib, rotation, projection) -> tensor` — undistort + rectify
  one camera's image
- `disparity(left, right, num_disparities=64, block_size=9, min_disparity=0) ->
  [H, W, 1] float32` — SGBM disparity in pixels, on rectified grayscale pairs
- `reproject_3d(disparity, q, mark_missing=true) -> [H, W, 3] float32` — 3D
  points from disparity via the Q matrix (invalid pixels get a huge Z)
- `RectifyMap(calib, rotation, projection, width, height)` — precomputed
  rectification: `.apply(img)` per frame instead of `rectify()` (which rebuilds
  its maps every call; use RectifyMap in live loops)

## Features and planar geometry

```roxal
var fa = orb_detect(img_a)          # Features{points [N,4] x,y,size,angle; descriptors [N,32]}
var fb = orb_detect(img_b)
var ms = match_features(fa, fb)     # Matches{pairs [M,2] indices; distances [M]}
```

- `find_homography(src_points, dst_points, method=HomographyMethod.Ransac,
  threshold=3.0) -> [3, 3]`
- `estimate_affine(src_points, dst_points) -> [2, 3]`
- `draw_matches(img_a, fa, img_b, fb, matches) -> tensor` — side-by-side match
  visualization for debugging feature pipelines

## DNN task wrappers

These wrap OpenCV's bundled task models (pre/post-processing included). They
need ONNX model files — **run `modules/opencv/models/download-models.sh` once**
(~55 MB). Default model paths resolve next to the module (via
`sys.source_dir()`), so scripts work from any directory; pass `model=` to any
constructor to override.

```roxal
var fd = FaceDetector()                       # YuNet
for face in fd.detect(img):                   # list of Face
  rectangle(img, face.box[0], face.box[1], face.box[2], face.box[3], [0, 255, 0], 2)
  # face.landmarks: float32 [5, 2] (eyes, nose, mouth corners); face.score
```

Learned features — a drop-in upgrade over `orb_detect`/`match_features` for
hard cases (viewpoint/lighting changes):

```roxal
var det = Aliked()                            # float32 [N, 128] descriptors
var fa = det.detect(img_a)
var fb = det.detect(img_b)
var ms = LightGlue().match_features(fa, fb)   # Matches, same shape as ORB path
```

Single-object tracking (VIT):

```roxal
var tr = Tracker()
tr.start(frame, x, y, w, h)                   # box around the target
var box = tr.update(next_frame)               # [x, y, w, h] or nil when lost
print(tr.score)                               # confidence of the last update
```

## Interop with ai.nn (onnxruntime)

opencv and `ai.nn` share tensors directly — the only impedance is layout:
images are uint8 `[H, W, C]`, DNN models want float32 `[1, 3, H, W]`.
`to_blob()` bridges it (resize + scale + normalize + transpose, zero
intermediate copies on the Roxal side):

```roxal
var blob = to_blob(frame, 640, 640)           # NCHW float32, 0..1
var result = model.predict(blob)              # ai.nn Model
# ImageNet-style normalization when a model needs it:
var blob2 = to_blob(frame, 224, 224, mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])
```

`examples/opencv-detect.rox` is the full loop: camera → `to_blob` → D-FINE
detection via ai.nn → box parsing → opencv drawing → FrameView display.

## The signals idiom (streaming pipelines)

For continuous processing, publish frames on a signal instead of hand-rolling
loops: a camera actor freezes each frame (`var pub: const tensor =
move(frame)`) and `set()`s it (zero-copy — const values share by reference);
a dataflow function derives the processed view; the UI just reacts to changes.
Tensor signals change-detect by content, and published frames are immutable to
samplers. `examples/opencv-signals.rox` is the complete pattern:

```roxal
const raw = signal(0, tensor([2, 2, 3], dtype='uint8'))   # event-driven

func annotate(frame: tensor) -> tensor:                    # dataflow node
  var img = clone(frame)                                   # frozen in, mutable copy
  ...canny / detect_markers / draw...
  return img

const view = annotate(raw)                                 # derived signal
when view changes as evt:
  fb.present(evt.value)
```

## Enums

Generated from OpenCV's own headers (values are never hand-transcribed):
`Interpolation`, `ColorCode`, `ThresholdType`, `MorphShape`, `MorphOp`,
`ContourMode`, `ContourApprox`, `Font`, `Rotation`, `CapProp`, `ArucoDict`,
`HandEyeMethod`, `TemplateMethod`; plus hand-defined `HomographyMethod`.

## Architecture, building, extending

See `README.md` in this directory: layout, the `shim/generate.py` generator
(enum groups + the same-shape filter family are generated from OpenCV's
`hdr_parser.py` metadata), `shim/build.sh`, and the pattern to follow when
adding new functions.
