// Fork shell switch: 36x20 track, 16px knob. Monochrome, like every selection: off is a dim
// track with a grey knob, on is a near-white track with a dark knob. Hover lightens the track
// a little; nothing glows. Emits switched(next) and leaves `on` to its binding, so the model
// stays the single source of truth.
import QtQuick
import QtQuick.Controls

AbstractButton {
    id: sw
    property bool on: false
    signal switched(bool on)
    implicitWidth: 36
    implicitHeight: 20
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.CheckBox
    Accessible.checked: on
    onClicked: switched(!on)
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: sw.on ? (sw.hovered ? Theme.solidHover : Theme.solid)
                         : Qt.rgba(1, 1, 1, sw.hovered ? 0.13 : 0.09)
            Behavior on color { ColorAnimation { duration: Theme.d(Theme.durState); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        }
        UiFocusRing { innerRadius: sw.height / 2; visible: sw.visualFocus }
    }
    contentItem: Item {
        Rectangle {
            width: 16
            height: 16
            radius: 8
            y: 2
            x: sw.on ? 18 : 2
            color: sw.on ? Theme.solidInk : Theme.t2
            scale: sw.pressed ? 0.88 : 1
            Behavior on x { NumberAnimation { duration: Theme.d(Theme.durState); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
            Behavior on color { ColorAnimation { duration: Theme.d(Theme.durState) } }
            Behavior on scale { NumberAnimation { duration: Theme.d(Theme.durHover); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
