// Fork shell segmented control. Options are [{ id, label, disabled? }]; each cell is as wide
// as its label (or `cellWidth` when set) and the selected indicator slides between cells.
// The indicator is a raised grey knob in a sunken rail (La Cuisine's .seg.glide), not an accent:
// selection is lit grey, the accent is kept for focus. The slide is skipped on first layout so a
// page never opens with a moving indicator.
// With no option selected (a mixed state) the indicator hides instead of lying.
import QtQuick
import QtQuick.Controls

FocusScope {
    id: seg
    property var options: []
    property string current: ""
    property real cellWidth: 0
    property int cellHeight: 28
    property real fontSize: Theme.fsMeta
    property string accessibleName: ""
    signal picked(string id)
    readonly property int index: { for (let i = 0; i < options.length; ++i) if (String(options[i].id) === String(current)) return i; return -1 }
    property bool settled: false

    implicitWidth: row.implicitWidth + 4
    implicitHeight: cellHeight + 4
    activeFocusOnTab: true
    Accessible.role: Accessible.PageTabList
    Accessible.name: accessibleName

    function step(dir) {
        let i = Math.max(0, index)
        for (let n = 0; n < options.length; ++n) {
            i = (i + dir + options.length) % options.length
            if (!options[i].disabled) { picked(String(options[i].id)); return }
        }
    }
    Keys.onLeftPressed: step(-1)
    Keys.onRightPressed: step(1)
    Component.onCompleted: Qt.callLater(() => seg.settled = true)

    Rectangle {
        anchors.fill: parent
        radius: Theme.rMd
        color: Theme.fieldBg
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.06)
        Rectangle { x: parent.radius; y: 1; width: parent.width - 2 * parent.radius; height: 1; color: Qt.rgba(0, 0, 0, 0.35) }
    }
    UiFocusRing { innerRadius: Theme.rMd; visible: seg.activeFocus && seg.focusReason !== Qt.MouseFocusReason }
    Item {
        id: indicator
        readonly property Item cell: seg.index >= 0 && cells.count > seg.index ? cells.itemAt(seg.index) : null
        opacity: cell !== null ? 1 : 0
        x: 2 + (cell ? cell.x : 0)
        y: 2
        width: cell ? cell.width : 0
        height: seg.cellHeight
        Behavior on x { enabled: seg.settled; NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        Behavior on width { enabled: seg.settled; NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(Theme.durState) } }
        Rectangle { anchors.fill: parent; anchors.topMargin: 1; anchors.bottomMargin: -1; radius: Theme.rSm; color: Theme.shadow }
        Rectangle {
            anchors.fill: parent
            radius: Theme.rSm
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.knobHi }
                GradientStop { position: 1; color: Theme.knobLo }
            }
            border.width: 1
            border.color: Theme.stroke
            Rectangle { x: 2; y: 1; width: parent.width - 4; height: 1; color: Theme.lit }
        }
    }
    Row {
        id: row
        x: 2
        y: 2
        Repeater {
            id: cells
            model: seg.options
            delegate: AbstractButton {
                id: cell
                required property var modelData
                required property int index
                readonly property bool selected: index === seg.index
                width: seg.cellWidth > 0 ? seg.cellWidth : label.implicitWidth + 24
                height: seg.cellHeight
                enabled: !modelData.disabled
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.label
                Accessible.checked: selected
                onClicked: { seg.forceActiveFocus(Qt.MouseFocusReason); seg.picked(String(modelData.id)) }
                background: null
                contentItem: Text {
                    id: label
                    text: cell.modelData.label
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: cell.selected ? Theme.t1 : !cell.enabled ? Theme.t3 : cell.hovered ? Theme.t1 : Theme.t2
                    opacity: cell.enabled ? 1 : 0.6
                    font.family: Theme.fontMono
                    font.pixelSize: seg.fontSize
                    Behavior on color { ColorAnimation { duration: Theme.d(Theme.durState) } }
                }
                HoverHandler { cursorShape: cell.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
            }
        }
    }
}
