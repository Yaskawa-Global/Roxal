# Roxal `qt` examples

QML/QtQuick UIs driven from Roxal. The `qt` module is **optional** and OFF by
default; build with it enabled and point CMake at a desktop Qt 6 install:

```sh
cmake -B build/ -DROXAL_ENABLE_QT=ON -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64
cmake --build build/ -j4
```

## hello — window opens, close it to exit

```sh
./build/roxal examples/qt/hello.rox          # needs an X11 display
QT_QPA_PLATFORM=offscreen ./build/roxal examples/qt/hello.rox   # headless
```

[hello.rox](hello.rox) creates a `qt.Engine`, loads [hello.qml](hello.qml) (whose
root is a `Window`), and calls `engine.run()`. `run()` blocks the script — while
the VM keeps pumping Qt cooperatively — until the window is closed (or QML calls
`Qt.quit()`), then the script continues and exits with a clean teardown.

`QT_QPA_PLATFORM=offscreen` is a Qt platform plugin that renders to an in-memory
surface instead of a real display, so the program runs without an X server (handy
for CI/headless). Remove it to see an actual window.
