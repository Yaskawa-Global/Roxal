import QtQuick
import QtQuick.Window

// A window that closes itself shortly after opening. Closing the last window
// emits QGuiApplication::lastWindowClosed, which unblocks Engine.run().
Window {
    visible: true
    width: 200
    height: 100
    title: "Roxal Qt P0"
    // Log a marker (asserted in qt_load_file.err) before closing, so the test
    // fails if run() never actually blocked to pump this timer.
    Timer { interval: 50; running: true; onTriggered: { console.log("qt-tick"); close() } }
}
