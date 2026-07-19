import QtQuick
import QtQuick.Window
import Roxal

Window {
    objectName: "win"
    visible: true
    width: 640
    height: 400
    title: "Roxal Raycaster — Doom M1"
    color: "black"

    signal key(int code, bool down, bool repeat)

    // 320x200 frame scaled up; smooth: false keeps crisp pixels
    FrameView {
        objectName: "fb"
        smooth: false
        anchors.fill: parent
    }

    Item {
        focus: true
        Keys.onPressed: (ev) => { key(ev.key, true, ev.isAutoRepeat); ev.accepted = true }
        Keys.onReleased: (ev) => { key(ev.key, false, ev.isAutoRepeat) }
    }
}
