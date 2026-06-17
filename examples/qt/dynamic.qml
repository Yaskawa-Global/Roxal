import QtQuick
import QtQuick.Window
import QtQuick.Controls

// Demo UI for runtime item creation: a button asks Roxal to spawn a marker into the Flow
// container (objectName "canvas"); Roxal compiles a qt.Component once and create()s an
// instance under the canvas on each click. The container and status carry objectNames so
// Roxal can reach them; the button emits a signal Roxal handles.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 380
    height: 440
    title: "Roxal + Qt — dynamic items"
    color: "#0f172a"

    signal addRequested()

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Button { text: "Add joint"; onClicked: win.addRequested() }

        Text {
            objectName: "status"
            color: "#93c5fd"
            font.pixelSize: 15
            text: "click “Add joint” to spawn markers"
        }

        Flow {
            id: canvas
            objectName: "canvas"
            width: parent.width
            spacing: 10
        }
    }
}
