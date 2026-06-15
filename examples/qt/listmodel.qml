import QtQuick
import QtQuick.Window
import QtQuick.Controls

// P3 demo UI: a ListView whose rows come from a Roxal qt.ListModel. Each Task's
// properties are the roles, so the delegate binds model.name / model.done. The
// checkbox writes model.done back (→ setData → the Roxal row), and the button asks
// Roxal (via a signal) to append a row. Items carry objectNames so Roxal/tests
// can find them.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 360
    height: 360
    title: "Roxal + Qt — P3 list model"
    color: "#0f172a"

    // Roxal listens for this and appends a row (Roxal → model, signals → slots).
    signal addRequested()

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Button {
            objectName: "addBtn"
            text: "Add task"
            onClicked: win.addRequested()
        }

        Text {
            objectName: "status"
            color: "#93c5fd"
            font.pixelSize: 16
            text: "tasks: " + list.count
        }

        ListView {
            id: list
            objectName: "list"
            width: parent.width
            height: 250
            clip: true
            model: tasks
            spacing: 6
            delegate: Row {
                spacing: 8
                CheckBox {
                    checked: model.done
                    onToggled: model.done = checked   // → setData → Roxal row.done
                }
                Text {
                    text: model.name
                    color: model.done ? "#4ade80" : "#e2e8f0"
                    font.pixelSize: 16
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
