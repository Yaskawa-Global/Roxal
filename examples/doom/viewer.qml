import QtQuick
import QtQuick.Window
import Roxal

Window {
    objectName: "win"
    visible: true
    width: 640
    height: 400
    title: "Roxal WAD viewer — Doom M2"
    color: "black"

    signal key(int code, bool down, bool repeat)

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
