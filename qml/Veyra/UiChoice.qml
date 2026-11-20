// One option of a radio list (hosts, consoles, devices, presets): the radio, a title, an
// optional mono detail or a sentence, and a trailing tag. The selected dot grows in from
// a quarter of its size.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AbstractButton {
    id: c
    property string title: ""
    property string detail: ""
    property string desc: ""
    property string tag: ""
    property color tagColor: selected ? Theme.accentText : Theme.t3
    property bool selected: false
    property bool showRadio: true
    property bool lastRow: false
    Layout.fillWidth: true
    implicitHeight: lay.implicitHeight + 32
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.RadioButton
    Accessible.name: title
    Accessible.checked: selected
    opacity: enabled ? 1 : 0.5

    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: -12
            anchors.rightMargin: -12
            anchors.bottomMargin: 1
            radius: Theme.rMd
            color: c.hovered ? Theme.rowHover : "transparent"
            border.width: c.visualFocus ? 2 : 0
            border.color: Theme.focusRing
            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    }
    contentItem: RowLayout {
        id: lay
        spacing: 16
        Item {
            visible: c.showRadio
            implicitWidth: 16
            implicitHeight: 16
            Layout.alignment: Qt.AlignVCenter
            Rectangle {
                anchors.fill: parent
                radius: 8
                color: "transparent"
                border.width: 1
                border.color: c.selected ? Theme.accent : Theme.line2
                Behavior on border.color { ColorAnimation { duration: Theme.d(200) } }
            }
            Rectangle {
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: Theme.accent
                opacity: c.selected ? 1 : 0
                scale: c.selected ? 1 : 0.25
                Behavior on opacity { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                Behavior on scale { NumberAnimation { duration: Theme.d(300); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: c.title
                color: Theme.t1
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsLead
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: c.detail.length > 0
                text: c.detail
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                font.features: { "tnum": 1 }
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: c.desc.length > 0
                text: c.desc
                color: Theme.t2
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsText
                lineHeight: 1.5
                wrapMode: Text.Wrap
            }
        }
        Text {
            visible: c.tag.length > 0
            text: c.tag
            color: c.tagColor
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            Behavior on color { ColorAnimation { duration: Theme.d(200) } }
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
