// Fork shell button (2026-11 mockups). kind: "solid" (the one primary action of a screen,
// near-white), "ghost" (a raised grey surface for secondary actions), "tool" (flat, toolbars and
// icon buttons). `active` marks a toggled tool button: lit grey, like any selection. Raised
// kinds carry a 1px top highlight and a 1px drop shadow. Press scales to 0.96; hover only
// changes colours, never size or position.
import QtQuick
import QtQuick.Controls

AbstractButton {
    id: b
    property string kind: "ghost"
    property string iconName: ""
    property real iconSize: 16
    property bool iconFilled: false
    property bool active: false
    property int size: 40
    property real fontSize: kind === "solid" ? Theme.fsCtl : Theme.fsMeta
    property string tip: ""
    // Nudges the icon on hover (the play triangle moves 2px towards where it leads).
    property real hoverNudge: 0
    property color inkOverride: "transparent"
    readonly property bool iconOnly: text.length === 0
    readonly property color ink: inkOverride.a > 0 ? inkOverride
                                : kind === "solid" ? Theme.solidInk
                                : active || hovered ? Theme.t1 : Theme.t2

    implicitHeight: size
    implicitWidth: iconOnly ? size : row.implicitWidth + (kind === "solid" ? 44 : 24)
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.4
    Accessible.name: text.length > 0 ? text : tip
    scale: pressed ? Theme.pressScale : 1
    Behavior on scale { NumberAnimation { duration: Theme.d(b.pressed ? 100 : 200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }

    background: Item {
        readonly property bool raised: b.kind !== "tool" && !b.active
        Rectangle {
            visible: parent.raised
            anchors.fill: face
            anchors.topMargin: 1
            anchors.bottomMargin: -1
            radius: face.radius
            color: Theme.shadow
        }
        Rectangle {
            id: face
            anchors.fill: parent
            radius: Theme.rMd
            color: b.kind === "solid" ? (b.hovered ? Theme.solidHover : Theme.solid)
                 : b.active ? Theme.selBg
                 : b.kind === "ghost" ? (b.hovered ? Theme.knobHi : Theme.knobLo)
                 : b.hovered ? Theme.hoverBg : "transparent"
            border.width: b.kind === "tool" && !b.active ? 0 : 1
            border.color: b.kind === "solid" ? Theme.ringStrong
                        : b.active ? Theme.selLine
                        : b.hovered ? Theme.stroke2 : Theme.stroke
            Behavior on color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
            Behavior on border.color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
            Rectangle {
                visible: b.kind === "ghost" || b.active
                x: face.radius
                y: 1
                width: face.width - 2 * face.radius
                height: 1
                color: Theme.lit
            }
        }
        UiFocusRing { innerRadius: Theme.rMd; visible: b.visualFocus }
    }
    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: b.kind === "solid" ? 12 : 8
            VIcon {
                visible: b.iconName.length > 0
                anchors.verticalCenter: parent.verticalCenter
                name: b.iconName
                size: b.iconSize
                filled: b.iconFilled
                strokeWidth: 1.5
                color: b.ink
                transform: Translate {
                    x: b.hovered ? b.hoverNudge : 0
                    Behavior on x { NumberAnimation { duration: Theme.d(Theme.durState); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                }
            }
            Text {
                visible: !b.iconOnly
                anchors.verticalCenter: parent.verticalCenter
                text: b.text
                color: b.ink
                font.family: Theme.fontMono
                font.pixelSize: b.fontSize
                font.weight: b.kind === "solid" ? Font.Medium : Font.Normal
                Behavior on color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
            }
        }
    }
    ToolTip.visible: tip.length > 0 && hovered
    ToolTip.delay: 600
    ToolTip.text: tip
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
