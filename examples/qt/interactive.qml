import QtQuick
import QtQuick.Window

// A small UI whose items carry objectNames so Roxal can find them, read/write
// their properties, and call their functions (P1). The Rectangle exposes a
// custom `count` property and a `bump()` function.
Window {
    objectName: "win"
    visible: true
    width: 380
    height: 260
    color: "#0f172a"
    title: "Roxal + Qt — P1"

    Column {
        anchors.centerIn: parent
        spacing: 18

        Text {
            objectName: "title"
            text: "(set by QML)"
            color: "white"
            font.pixelSize: 22
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Rectangle {
            objectName: "box"
            width: 140
            height: 100
            radius: 10
            color: "gray"
            anchors.horizontalCenter: parent.horizontalCenter

            property int count: 0
            function bump() { count = count + 1; return count }

            Text {
                anchors.centerIn: parent
                text: parent.count
                color: "white"
                font.pixelSize: 36
            }
        }

        Text {
            objectName: "status"
            text: ""
            color: "#93c5fd"
            font.pixelSize: 13
            anchors.horizontalCenter: parent.horizontalCenter
        }
    }
}
