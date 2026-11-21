// Partager (fork): while guests are connected this chip stays on the picture, top centre, so the
// host always knows who sees the stream and who drives the pad. A click opens the panel.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: chip
    property bool preview: false
    signal clicked()
    readonly property bool videoCover: true
    property real coverRadius: Theme.rLg
    readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
    readonly property bool shown: preview || (g !== null && g.phase === "connected")
    readonly property var room: preview ? ({ holderName: "lucas", count: 3, owner: false }) : g ? g.room : ({})
    readonly property string mode: preview ? "guest" : g ? g.mode : "guest"
    width: row.implicitWidth + 24
    height: 30
    radius: Theme.rLg
    color: Theme.popover
    border.width: 1
    border.color: Theme.stroke2
    visible: opacity > 0.01
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.d(220); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    Accessible.role: Accessible.StaticText
    Accessible.name: label.text
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8
        VIcon { name: "users"; size: 14; strokeWidth: 1.5; color: Theme.t2 }
        Text {
            id: label
            text: qsTr("%1 invité(s)").arg(chip.room.count || 0) + " · "
                  + (chip.mode === "watch" ? qsTr("ils regardent") : chip.room.owner ? qsTr("tu as la manette")
                     : chip.room.holderName ? qsTr("%1 joue").arg(chip.room.holderName) : qsTr("manette libre"))
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
        }
    }
    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: chip.clicked() }
}
