import QtQuick
import QtQuick.Window
import Roxal

Window {
    visible: true
    width: 640
    height: 400
    title: "Roxal FrameView — software framebuffer"
    color: "black"

    // The frame is 320x200; the scene graph scales it up. smooth: false keeps
    // the crisp pixels (nearest-neighbor) instead of blurring them.
    FrameView {
        objectName: "fb"
        smooth: false
        anchors.fill: parent
    }
}
