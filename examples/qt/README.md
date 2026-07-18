# Roxal `qt` examples

> ⚠️ **Experimental.** The `qt` module is new and not yet exercised by any large
> application, so expect missing features, rough edges, and latent bugs. The API may
> change.

QML/QtQuick UIs driven from Roxal. The `qt` module is **optional** and OFF by
default; build with it enabled and point CMake at a desktop Qt 6 install:

```sh
cmake -B build/ -DROXAL_ENABLE_QT=ON -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64
cmake --build build/ -j4
```

Each example runs the same two ways — on a real X11 display, or headless on Qt's
offscreen platform plugin (renders to an in-memory surface, no X server — handy for
CI):

```sh
./build/roxal examples/qt/hello.rox                              # real window
QT_QPA_PLATFORM=offscreen ./build/roxal examples/qt/hello.rox    # headless
```

`engine.run()` blocks the script — while the VM keeps pumping Qt cooperatively —
until the window closes (or QML calls `Qt.quit()`), then the script continues and
exits with a clean teardown.

## The examples

Each builds on the one before, covering a capability of the module. For a narrative
walk-through see [roxal-qt-module-guide.md](roxal-qt-module-guide.md).

| Example | Shows | Try it |
| --- | --- | --- |
| [hello](hello.rox) | Engine lifecycle: load a `Window`, `run()`, clean teardown | Window opens; close it to exit. |
| [interactive](interactive.rox) | **P1** — find items by `objectName`; read/write properties; call a QML method | At startup Roxal sets the title to *"Driven from Roxal!"*, recolors the box, and calls `bump()` 3× → box shows **3**. |
| [signals](signals.rox) | **P2** — a Qt signal reaches a Roxal callback (`btn.onClicked(...)`) | Click **Click me** → status counts up (*clicks: 1, 2, …*). |
| [listmodel](listmodel.rox) | **P3** — a `ListView` backed by a `qt.ListModel` of Roxal row objects | Click **Add task** → a row is appended; tick a checkbox → the edit flows back to the row. |
| [bindable](bindable.rox) | **P4** — a Roxal object exposed as a QML-bindable backend | Drag the slider → *Volume: N* (QML→Roxal); click **Reset from Roxal** → title + volume reset (Roxal→QML auto-push). |
| [sortfilter](sortfilter.rox) | A `qt.SortFilterModel` view over a list model | Type in the filter → the list narrows; click **Sort by name** / **Priority (high→low)** to reorder. |
| [treeview](treeview.rox) | A `qt.TreeModel` in a QML `TreeView` | Click **Add joint to the tip** → a node is appended down the chain; expand/collapse to see it. |
| [dynamic](dynamic.rox) | **Runtime creation** — `engine.create_component` + `Component.create` | Click **Add joint** → a numbered marker is spawned into the container. |
| [framebuffer](framebuffer.rox) | **Pixel frames** — a `FrameView` presents uint8 `[H, W, C]` tensors; CPU-only rendering via `qt.set_render_backend("software")` | A palette-cycling plasma animates at ~12 fps, chunky nearest-neighbor pixels — no GPU used. |
| [teapot](teapot.rox) | **3D** — Qt Quick 3D renders the Utah teapot (`RuntimeLoader`); drag to orbit | **Cycle tint** / **Pause** the spin (Roxal-driven); needs an OpenGL display. |
| [gallery](gallery.rox) | **Widget gallery** — a tabbed `ApplicationWindow` showing off many Qt Quick Controls + visual items | Click the **Inputs / Lists / Table / Gallery / Log** tabs; every interaction is logged by Roxal (footer + Log tab). |

The 3D example uses **Qt Quick 3D** (the modern Qt 6 3D module — not the older Qt 3D /
`Qt3DExtras`). The mesh [teapot.obj](teapot.obj) (the Utah/Newell teapot) is loaded at runtime by
`QtQuick3D.AssetUtils.RuntimeLoader`, and Qt Quick 3D is pulled in purely as a QML import — so the
`roxal` binary stays Qt-free, exactly like the 2D examples. It needs a real OpenGL-capable display
(it won't render under `QT_QPA_PLATFORM=offscreen`).

The **gallery** is the broad tour: a `TabBar` over a `StackLayout`, an `ApplicationWindow` header/
footer status bar, and across the tabs — `TextField`, `ComboBox`, `SpinBox`, `Slider`, `Switch`,
`CheckBox`, `RadioButton`, `GroupBox`, `Frame`; a `ListView` over a Roxal `qt.ListModel`; a
`TableView` + `HorizontalHeaderView` over a `TableModel`; gradient/rounded/bordered `Rectangle`s,
`ProgressBar` (incl. indeterminate), `BusyIndicator`, `PageIndicator`, `Dial`, `RangeSlider`,
`Tumbler`; and a `ScrollView` + `TextArea` event log. Roxal owns the list model and logs every
interaction to the footer and the Log tab.

## Smoke-testing the UI paths

The headless `qt_*` tests (`python3 runtests.py -t 'qt_*'`,
`QT_QPA_PLATFORM=offscreen`) cover these paths automatically. To additionally
exercise them on a **real display with real input** — clicks, typing, drags — drive
the examples above and confirm the UI reacts as the *Try it* column describes.

This repo's `gui-user` computer-use server makes that scriptable: it launches an app
on a private virtual display (Xvfb), then observes via screenshots / the
accessibility tree and injects input. A run looks like:

1. `launch_app("./build/roxal", args=["examples/qt/dynamic.rox"], working_dir=<repo>, vnc=true)`
2. `screenshot()` — confirm the window rendered
3. `click_element("Add joint")` (or `click(x, y)` from the screenshot) — drive it
4. `screenshot()` — confirm the UI changed (a new marker, an updated count, …)
5. `close_app()` between examples; `stop_display()` when done

Screenshots auto-save under `.gui-user/screenshots/` (git-ignored scratch).
