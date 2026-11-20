// A dropdown for long option lists (devices, resolutions, algorithms). Options are
// [{ id, label, disabled? }]; `current` is the selected id. The list opens under the button
// with a 150 ms fade and scale from 0.96, and closes on a pick, Escape or a click outside.
import QtQuick
import QtQuick.Controls

AbstractButton {
    id: sel
    property var options: []
    property string current: ""
    property string placeholder: qsTr("choisir")
    signal picked(string id)
    readonly property var chosen: options.find(o => String(o.id) === String(current))
    implicitWidth: 220
    implicitHeight: 32
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.ComboBox
    Accessible.name: chosen ? chosen.label : placeholder
    opacity: enabled ? 1 : 0.4
    onClicked: popup.opened ? popup.close() : popup.open()

    background: Rectangle {
        radius: Theme.rMd
        color: sel.hovered || popup.opened ? Theme.hoverBg : Theme.fieldBg
        border.width: 1
        border.color: popup.opened || sel.hovered ? Theme.stroke2 : Theme.stroke
        Behavior on color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
        UiFocusRing { innerRadius: Theme.rMd; visible: sel.visualFocus }
    }
    contentItem: Item {
        Text {
            anchors.left: parent.left
            anchors.right: chevron.left
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: sel.chosen ? sel.chosen.label : sel.placeholder
            color: sel.chosen ? Theme.t1 : Theme.t3
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            elide: Text.ElideRight
        }
        VIcon {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            name: "down"
            size: 14
            strokeWidth: 1.5
            color: Theme.t2
            rotation: popup.opened ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }

    Popup {
        id: popup
        y: sel.height + 4
        width: Math.max(sel.width, 200)
        height: Math.min(list.contentHeight + 8, 320)
        padding: 4
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        transformOrigin: Popup.Top
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.d(150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.d(150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: Theme.d(120) }
        }
        background: Rectangle {
            radius: Theme.rLg
            color: Theme.popover
            border.width: 1
            border.color: Theme.stroke2
        }
        contentItem: ListView {
            id: list
            clip: true
            model: sel.options
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: VScrollBar {}
            delegate: AbstractButton {
                id: opt
                required property var modelData
                width: ListView.view.width
                height: 32
                enabled: !modelData.disabled
                hoverEnabled: true
                onClicked: { popup.close(); sel.picked(String(modelData.id)) }
                background: Rectangle {
                    radius: Theme.rMd
                    color: opt.hovered ? Theme.hoverBg : "transparent"
                }
                contentItem: Text {
                    leftPadding: 8
                    text: opt.modelData.label
                    verticalAlignment: Text.AlignVCenter
                    color: String(opt.modelData.id) === String(sel.current) ? Theme.t1 : opt.enabled ? Theme.t1 : Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                    elide: Text.ElideRight
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }
}
