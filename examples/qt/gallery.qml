import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.qmlmodels

// A wide "widget gallery": an ApplicationWindow with a TabBar header, a status-bar footer,
// and a page per category showing off Qt Quick Controls + visual items. It is driven from
// Roxal: the Lists page is backed by a qt.ListModel Roxal owns, and every interaction emits
// a signal that Roxal logs (and reflects in the footer + the Log page).
//
// Each StackLayout page is a plain Item (a StackLayout sizes its children to fill it, so a
// child must NOT also anchors.fill — that would fight the layout and spin a polish() loop);
// the page content anchors-fills inside that Item.
ApplicationWindow {
    id: win
    objectName: "win"
    visible: true
    width: 960
    height: 680
    title: "Roxal × Qt Quick — widget gallery"

    // UI -> Roxal.
    signal logRequested(string msg)     // "something happened" — Roxal logs it
    signal addItemRequested(string text) // add a row to the Lists model (Roxal owns it)

    // Roxal -> UI (called from Roxal handlers).
    function logLine(s) {
        logArea.text += s + "\n"
        logArea.cursorPosition = logArea.length
    }

    header: TabBar {
        id: bar
        TabButton { text: "Inputs" }
        TabButton { text: "Lists" }
        TabButton { text: "Table" }
        TabButton { text: "Gallery" }
        TabButton { text: "Log" }
    }

    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            Label { id: status; objectName: "status"; text: "Ready"; leftPadding: 12 }
            Item { Layout.fillWidth: true }
            Label { text: "Roxal × Qt Quick Controls"; rightPadding: 12; opacity: 0.65 }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: bar.currentIndex

        // ============================== Inputs ==============================
        Item {
            ScrollView {
                id: inputsScroll
                anchors.fill: parent
                contentWidth: availableWidth
                ColumnLayout {
                    width: inputsScroll.availableWidth
                    spacing: 14

                    GroupBox {
                        title: "Profile"
                        Layout.fillWidth: true
                        Layout.margins: 12
                        GridLayout {
                            anchors.fill: parent
                            columns: 2
                            columnSpacing: 14
                            rowSpacing: 10
                            Label { text: "Name" }
                            TextField {
                                id: nameField
                                Layout.fillWidth: true
                                placeholderText: "your name"
                                onEditingFinished: if (text !== "") win.logRequested("name = " + text)
                            }
                            Label { text: "Role" }
                            ComboBox {
                                id: roleBox
                                Layout.fillWidth: true
                                model: ["Operator", "Engineer", "Administrator"]
                                onActivated: win.logRequested("role = " + currentText)
                            }
                            Label { text: "Count" }
                            SpinBox {
                                id: countSpin
                                from: 0; to: 100; value: 4
                                onValueModified: win.logRequested("count = " + value)
                            }
                            Label { text: "Level" }
                            RowLayout {
                                Layout.fillWidth: true
                                Slider {
                                    id: levelSlider
                                    Layout.fillWidth: true
                                    from: 0; to: 100; value: 50
                                    onMoved: win.logRequested("level = " + Math.round(value))
                                }
                                Label { text: Math.round(levelSlider.value); Layout.preferredWidth: 32 }
                            }
                            Label { text: "Enabled" }
                            Switch {
                                id: enabledSwitch
                                checked: true
                                onToggled: win.logRequested("enabled = " + checked)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: 12
                        spacing: 14

                        GroupBox {
                            title: "Notifications"
                            Layout.fillWidth: true
                            ColumnLayout {
                                CheckBox { text: "Email"; checked: true; onToggled: win.logRequested("email " + (checked ? "on" : "off")) }
                                CheckBox { text: "SMS"; onToggled: win.logRequested("sms " + (checked ? "on" : "off")) }
                                CheckBox { text: "Push"; onToggled: win.logRequested("push " + (checked ? "on" : "off")) }
                            }
                        }

                        GroupBox {
                            title: "Theme"
                            Layout.fillWidth: true
                            ColumnLayout {
                                RadioButton { text: "Light"; checked: true; onToggled: if (checked) win.logRequested("theme = light") }
                                RadioButton { text: "Dark"; onToggled: if (checked) win.logRequested("theme = dark") }
                                RadioButton { text: "System"; onToggled: if (checked) win.logRequested("theme = system") }
                            }
                        }
                    }

                    Button {
                        text: "Apply"
                        Layout.leftMargin: 12
                        Layout.bottomMargin: 12
                        onClicked: win.logRequested("Apply: " + nameField.text + " (" + roleBox.currentText + ")")
                    }
                }
            }
        }

        // ============================== Lists ==============================
        Item {
            ColumnLayout {
                anchors.fill: parent

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: 12
                    TextField {
                        id: newItem
                        Layout.fillWidth: true
                        placeholderText: "new part name"
                        onAccepted: { win.addItemRequested(text); clear() }
                    }
                    Button { text: "Add"; onClicked: { win.addItemRequested(newItem.text); newItem.clear() } }
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 12
                    ListView {
                        id: partsView
                        anchors.fill: parent
                        clip: true
                        model: parts                 // a qt.ListModel exposed by Roxal
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            width: ListView.view.width
                            text: (index + 1) + ".   " + model.name + "      × " + model.qty
                            highlighted: ListView.isCurrentItem
                            onClicked: {
                                partsView.currentIndex = index
                                win.logRequested("selected " + model.name)
                            }
                        }
                        highlight: Rectangle { color: "#3b82f6"; opacity: 0.18 }
                    }
                }
            }
        }

        // ============================== Table ==============================
        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 0

                HorizontalHeaderView {
                    id: hHeader
                    Layout.fillWidth: true
                    syncView: inventory
                    clip: true
                    delegate: Rectangle {
                        implicitHeight: 34
                        color: "#1f2937"
                        Label {
                            anchors.centerIn: parent
                            color: "#e5e7eb"
                            font.bold: true
                            text: ["Part", "Qty", "Status"][index]
                        }
                    }
                }

                TableView {
                    id: inventory
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    columnSpacing: 1
                    rowSpacing: 1
                    ScrollBar.vertical: ScrollBar {}
                    model: TableModel {
                        TableModelColumn { display: "name" }
                        TableModelColumn { display: "qty" }
                        TableModelColumn { display: "status" }
                        rows: [
                            { name: "Gripper",        qty: 3, status: "OK" },
                            { name: "Base plate",     qty: 5, status: "OK" },
                            { name: "Wrist sensor",   qty: 1, status: "Low" },
                            { name: "Shoulder motor", qty: 4, status: "OK" },
                            { name: "Elbow joint",    qty: 2, status: "Check" },
                            { name: "Coolant pump",   qty: 0, status: "Out" }
                        ]
                    }
                    delegate: Rectangle {
                        implicitWidth: 150
                        implicitHeight: 36
                        color: row % 2 ? "#0f172a" : "#111c33"
                        Label {
                            anchors.centerIn: parent
                            color: (column === 2 && (display === "Low" || display === "Out")) ? "#f87171" : "#dbeafe"
                            text: display
                        }
                    }
                }
            }
        }

        // ============================== Gallery ==============================
        Item {
            ScrollView {
                id: galleryScroll
                anchors.fill: parent
                contentWidth: availableWidth
                Flow {
                    width: galleryScroll.availableWidth
                    padding: 16
                    spacing: 18

                    // Gradient card with rounded corners + border
                    Rectangle {
                        width: 240; height: 140; radius: 18
                        border.color: "#475569"; border.width: 2
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#7c3aed" }
                            GradientStop { position: 1.0; color: "#db2777" }
                        }
                        Label {
                            anchors.centerIn: parent
                            text: "Gradient\n+ rounded + border"
                            horizontalAlignment: Text.AlignHCenter
                            color: "white"; font.bold: true
                        }
                    }

                    // Progress + indicators
                    Frame {
                        width: 240; height: 140
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8
                            Label { text: "Progress / busy" }
                            ProgressBar { id: pbar; Layout.fillWidth: true; from: 0; to: 100; value: levelSlider.value }
                            ProgressBar { Layout.fillWidth: true; indeterminate: true }
                            RowLayout {
                                BusyIndicator { running: true; implicitWidth: 40; implicitHeight: 40 }
                                PageIndicator { count: 5; currentIndex: 2 }
                            }
                        }
                    }

                    // Dial + range slider
                    Frame {
                        width: 240; height: 140
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "Dial / range" }
                            RowLayout {
                                Dial { from: 0; to: 100; value: 35; implicitWidth: 80; implicitHeight: 80 }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    RangeSlider { Layout.fillWidth: true; from: 0; to: 100; first.value: 25; second.value: 75 }
                                    Label { text: "RangeSlider"; opacity: 0.7 }
                                }
                            }
                        }
                    }

                    // A circle (radius = half) + swatches
                    Rectangle {
                        width: 240; height: 140; radius: 14; color: "#0b1220"; border.color: "#334155"
                        RowLayout {
                            anchors.centerIn: parent
                            spacing: 12
                            Rectangle { width: 70; height: 70; radius: 35
                                gradient: Gradient {
                                    GradientStop { position: 0; color: "#22d3ee" }
                                    GradientStop { position: 1; color: "#2563eb" }
                                }
                            }
                            ColumnLayout {
                                Rectangle { width: 60; height: 18; radius: 9; color: "#f59e0b" }
                                Rectangle { width: 60; height: 18; radius: 9; color: "#10b981" }
                                Rectangle { width: 60; height: 18; radius: 9; color: "#ef4444" }
                            }
                        }
                    }

                    // Tumbler
                    Frame {
                        width: 240; height: 140
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "Tumbler" }
                            Tumbler {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                model: 12
                            }
                        }
                    }
                }
            }
        }

        // ============================== Log ==============================
        Item {
            ScrollView {
                anchors.fill: parent
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                TextArea {
                    id: logArea
                    readOnly: true
                    wrapMode: TextArea.Wrap
                    text: "— event log —\n"
                }
            }
        }
    }
}
