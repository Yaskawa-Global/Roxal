import QtQuick
import QtQuick.Window
import QtQuick.Controls

// P2 demo UI: a Button whose `clicked` signal Roxal connects to, and a status
// Text that Roxal updates in response. Items carry objectNames so Roxal can find
// them.
Window {
    objectName: "win"
    visible: true
    width: 360
    height: 220
    title: "Roxal + Qt — P2 signals"
    color: "#0f172a"

    Column {
        anchors.centerIn: parent
        spacing: 24

        Button {
            objectName: "btn"
            text: "Click me"
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            objectName: "status"
            text: "no clicks yet"
            color: "#93c5fd"
            font.pixelSize: 20
            anchors.horizontalCenter: parent.horizontalCenter
        }
    }
}
