import QtQuick
import QtQuick.Window
import Roxal

Window {
    objectName: "win"
    visible: true
    width: 1280
    height: 960    // 320x200 at 4:3 like the original; Qt scales the
                   // framebuffer, so window size doesn't affect frame cost
    title: "Roxal Doom"
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
