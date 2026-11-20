// A labelled slider: the label and its live value on one line, a 4px track below. The value
// is committed on release (and on each keyboard step), not on every pointer move, so a
// preference is written once per gesture.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

ColumnLayout {
    id: s
    property string label: ""
    property real from: 0
    property real to: 100
    property real stepSize: 1
    property real value: 0
    property string suffix: ""
    property var format: v => String(Math.round(v))
    signal committed(real value)
    spacing: 8
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: s.label
            color: Theme.t1
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsText
            elide: Text.ElideRight
        }
        Text {
            text: s.format(slider.value) + s.suffix
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            font.features: { "tnum": 1 }
        }
    }
    Basic.Slider {
        id: slider
        Layout.fillWidth: true
        implicitHeight: 20
        padding: 0
        from: s.from
        to: s.to
        stepSize: s.stepSize
        snapMode: Slider.SnapAlways
        value: s.value
        Accessible.name: s.label
        onPressedChanged: if (!pressed) s.committed(value)
        onMoved: if (!pressed) s.committed(value)
        background: Item {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - 2
            width: slider.availableWidth
            height: 4
            Rectangle { anchors.fill: parent; radius: 2; color: Theme.line2 }
            Rectangle { width: slider.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.t2 }
        }
        handle: Item {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - 14)
            y: slider.topPadding + slider.availableHeight / 2 - 7
            width: 14
            height: 14
            Rectangle { anchors.fill: knob; anchors.topMargin: 1; anchors.bottomMargin: -1; radius: 7; color: Theme.shadow; scale: knob.scale }
            Rectangle {
                id: knob
                anchors.fill: parent
                radius: 7
                color: slider.hovered || slider.pressed ? Theme.solidHover : Theme.t1
                scale: slider.pressed ? 1.15 : 1
                Behavior on scale { NumberAnimation { duration: Theme.d(Theme.durHover); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                Behavior on color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
            }
            UiFocusRing { innerRadius: 7; visible: slider.visualFocus }
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
}
