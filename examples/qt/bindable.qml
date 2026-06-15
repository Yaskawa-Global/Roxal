import QtQuick
import QtQuick.Window
import QtQuick.Controls

// P4 demo UI: a Roxal object exposed to QML as a bindable backend (`app`). Its
// properties are bound directly — the label tracks app.title, the slider both reads
// and writes app.volume — and a button asks Roxal (via a signal) to mutate `app`,
// which auto-pushes back to these bindings.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 360
    height: 260
    title: "Roxal + Qt — P4 bindable object"
    color: "#0f172a"

    signal resetRequested()

    Column {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        Text {
            objectName: "titleLabel"
            text: app.title                    // ← Roxal app.title
            color: "#e2e8f0"
            font.pixelSize: 22
        }

        Text {
            objectName: "volumeLabel"
            text: "Volume: " + app.volume       // ← Roxal app.volume
            color: "#93c5fd"
            font.pixelSize: 16
        }

        Slider {
            objectName: "volumeSlider"
            width: parent.width
            from: 0; to: 100
            stepSize: 1                          // integer volume
            value: app.volume                   // ← reads app.volume
            onMoved: app.volume = value         // → writes app.volume (setData/gated)
        }

        Button {
            objectName: "resetBtn"
            text: "Reset from Roxal"
            onClicked: win.resetRequested()     // → Roxal mutates app.* (auto-pushes back)
        }
    }
}
