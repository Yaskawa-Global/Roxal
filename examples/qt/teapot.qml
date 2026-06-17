import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick3D
import QtQuick3D.AssetUtils
import QtQuick3D.Helpers

// A 3D scene using Qt Quick 3D — the modern Qt 6 3D facility (NOT the older Qt 3D /
// Qt3DExtras framework). The famous Utah teapot mesh (teapot.obj) is loaded at runtime
// by QtQuick3D.AssetUtils.RuntimeLoader; QML animates its rotation; Roxal drives the
// scene through signals/properties (toggle the spin, cycle a lighting tint). Qt Quick 3D
// is pulled in purely as a QML import, so the roxal binary needs nothing extra linked.
Window {
    id: win
    objectName: "win"
    visible: true
    width: 680
    height: 560
    title: "Roxal + Qt Quick 3D — Utah teapot"
    color: "#0b1020"

    property bool spinning: true

    signal cycleRequested()
    signal spinRequested()

    // Roxal recolors the scene by tinting the key light (RuntimeLoader uses a default
    // material, so we tint via light rather than reaching into the mesh's material).
    function applyTint(c) { keyLight.color = c }

    View3D {
        id: view
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#0b1020"
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        // Mouse camera control: left-drag to orbit, wheel to zoom, right-drag to pan.
        // The controller orbits the camera around `orbitOrigin` (the teapot's centre).
        OrbitCameraController {
            anchors.fill: parent
            origin: orbitOrigin
            camera: cam
        }

        Node {
            id: orbitOrigin
            position: Qt.vector3d(0, 0.4, 0)
            PerspectiveCamera {
                id: cam
                position: Qt.vector3d(0, 2.2, 11.0)   // relative to the orbit origin
                eulerRotation.x: -11
                clipNear: 0.1
                clipFar: 1000
            }
        }

        DirectionalLight {
            id: keyLight
            eulerRotation.x: -38
            eulerRotation.y: -35
            brightness: 1.35
        }
        DirectionalLight {           // cool fill from the other side
            eulerRotation.x: -6
            eulerRotation.y: 140
            color: "#a9c4ff"
            brightness: 0.4
        }

        Node {
            id: spinNode
            NumberAnimation on eulerRotation.y {
                from: 0; to: 360; duration: 12000
                loops: Animation.Infinite; running: win.spinning
            }

            RuntimeLoader {
                id: loader
                source: "teapot.obj"     // resolved next to this .qml
                y: -1.4                  // sit the body in frame
            }
        }
    }

    // Controls (plain Quick items overlaid on the View3D).
    Row {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 16
        spacing: 10
        Button { text: "Cycle tint"; onClicked: win.cycleRequested() }
        Button { text: win.spinning ? "Pause" : "Spin"; onClicked: win.spinRequested() }
    }

    Text {
        objectName: "status"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        color: "#cbd5e1"
        font.pixelSize: 15
        text: "Qt Quick 3D · Utah teapot — drag to orbit, wheel to zoom"
    }
}
