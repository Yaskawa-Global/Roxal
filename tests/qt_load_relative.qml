import QtQuick
import QtQuick.Window
Window {
    objectName: "win"
    visible: true
    Timer { interval: 30; running: true; onTriggered: Qt.quit() }
}
