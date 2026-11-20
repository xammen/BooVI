import QtQuick
import QtQuick.Layouts

// A new BooVI release: a small card in the bottom-right corner, never over the game. While a
// stream shows (held) it waits, and it appears once the stream page is left.
Rectangle {
    id: banner
    // The native video host subtracts visible videoCover items from its region.
    objectName: "videoCover"
    property real coverRadius: Theme.rLg
    property string version: ""
    property bool shown: false
    property bool held: false
    readonly property bool open: shown && !held
    signal openRequested()
    width: col.implicitWidth + 32
    height: col.implicitHeight + 28 + 1
    radius: Theme.rLg
    color: Theme.popover
    border.width: 1
    border.color: Theme.stroke2
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    transformOrigin: Item.BottomRight
    transform: Translate {
        x: banner.open || Theme.reduced ? 0 : 12
        y: banner.open || Theme.reduced ? 0 : 16
        Behavior on x { NumberAnimation { duration: Theme.d(banner.open ? 260 : 180); easing.type: banner.open ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        Behavior on y { NumberAnimation { duration: Theme.d(banner.open ? 260 : 180); easing.type: banner.open ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
    }
    Behavior on opacity { NumberAnimation { duration: Theme.d(banner.open ? 220 : 180); easing.type: banner.open ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
    function show(value) { version = value; shown = true }
    function dismiss() { shown = false }
    // Counts only while the card is on screen, so a release found mid-stream is still seen.
    Timer { interval: 10000; running: banner.open; onTriggered: banner.shown = false }
    Rectangle { x: parent.radius; y: 1; width: parent.width - 2 * parent.radius; height: 1; color: Theme.lit }
    ColumnLayout {
        id: col
        x: 16
        y: 15
        spacing: 10
        RowLayout {
            spacing: 8
            Text { text: qsTr("BooVI %1 est disponible").arg(banner.version); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsRow; textFormat: Text.PlainText }
            Item { Layout.fillWidth: true; Layout.minimumWidth: 12 }
            UiButton { objectName: "update-dismiss"; kind: "tool"; size: 24; iconSize: 12; iconName: "winclose"; tip: qsTr("fermer"); onClicked: banner.dismiss() }
        }
        UiButton {
            kind: "solid"
            size: 28
            fontSize: Theme.fsMeta
            text: qsTr("voir la version")
            onClicked: { banner.openRequested(); banner.dismiss() }
        }
    }
}
