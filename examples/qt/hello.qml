import QtQuick
import QtQuick.Window

// Hello-world UI for the Roxal `qt` module. The root is a Window (required by
// QQmlApplicationEngine). Closing it unblocks the Roxal script's engine.run().
Window {
    visible: true
    width: 360
    height: 200
    title: "Roxal · Qt Hello World"

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#1e3a8a" }
            GradientStop { position: 1.0; color: "#0f172a" }
        }

        Column {
            anchors.centerIn: parent
            spacing: 10

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Hello from Roxal + Qt!"
                color: "white"
                font.pixelSize: 22
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Close this window to exit."
                color: "#93c5fd"
                font.pixelSize: 13
            }
        }
    }
}
