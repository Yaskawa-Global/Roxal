import QtQuick
import QtQuick.Window
import QtQuick.Controls

// Demo UI: a ListView backed by a Roxal qt.SortFilterModel (a sorted/filtered view over a
// qt.ListModel). The text field filters by the `name` role and the buttons re-sort — but the
// sort/filter operations are Roxal builtins, so the UI asks Roxal (via signals) to apply them;
// Roxal calls view.filter(...) / view.sort_by(...) and the list updates live. Items carry
// objectNames so Roxal/tests can find them.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 360
    height: 460
    title: "Roxal + Qt — sort & filter"
    color: "#0f172a"

    signal filterChanged(string text)
    signal sortRequested(string role, bool descending)

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        TextField {
            objectName: "filterField"
            width: parent.width
            placeholderText: "Filter by name…"
            onTextChanged: win.filterChanged(text)
        }

        Row {
            spacing: 8
            Button { objectName: "sortName"; text: "Sort by name";     onClicked: win.sortRequested("name", false) }
            Button { objectName: "sortPrio"; text: "Priority (high→low)"; onClicked: win.sortRequested("priority", true) }
        }

        Text {
            objectName: "status"
            color: "#93c5fd"
            font.pixelSize: 15
            text: "showing " + list.count + " part(s)"
        }

        ListView {
            id: list
            objectName: "list"
            width: parent.width
            height: 320
            clip: true
            model: items                  // the qt.SortFilterModel
            spacing: 6
            delegate: Row {
                spacing: 10
                Text { text: model.priority; color: "#fbbf24"; font.pixelSize: 16; font.bold: true }
                Text { text: model.name;     color: "#e2e8f0"; font.pixelSize: 16 }
            }
        }
    }
}
