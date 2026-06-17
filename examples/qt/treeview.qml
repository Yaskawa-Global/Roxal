import QtQuick
import QtQuick.Window
import QtQuick.Controls

// Demo UI: a QML TreeView backed by a Roxal qt.TreeModel. Each Node is a Roxal object;
// its `children` list is the tree structure and its other public properties (here `name`)
// are the roles, so the delegate binds the `name` role. The "Add joint" button asks Roxal
// (via a signal) to append a child node — the tree grows live. Items carry objectNames so
// Roxal/tests can find them.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 360
    height: 440
    title: "Roxal + Qt — tree model"
    color: "#0f172a"

    // Roxal listens for this and appends a child node (Roxal → model → view).
    signal growRequested()

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Button {
            objectName: "growBtn"
            text: "Add joint to the tip"
            onClicked: win.growRequested()
        }

        Text {
            objectName: "status"
            color: "#93c5fd"
            font.pixelSize: 15
            text: "visible nodes: " + tv.rows
        }

        TreeView {
            id: tv
            objectName: "tv"
            width: parent.width
            height: 340
            clip: true
            model: scene

            // A custom delegate: TreeViewDelegate handles the expand/collapse arrow and
            // indentation; we declare the `name` role as a required property and show it.
            delegate: TreeViewDelegate {
                required property string name
                contentItem: Text {
                    text: name
                    color: "#0f172a"            // dark — readable on the light row background
                    font.pixelSize: 16
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Start fully expanded so the whole kinematic chain is visible.
            Component.onCompleted: tv.expandRecursively()
        }
    }
}
