// One settings row: a title, an optional one-line reason, and its control on the trailing
// edge. Rows stack inside a UiCard; every row but the first draws the divider above it.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: row
    property string title: ""
    property string desc: ""
    property bool first: false
    property color descColor: Theme.t2
    default property alias controls: slot.data
    Layout.fillWidth: true
    implicitHeight: Math.max(lay.implicitHeight + 32, 56)
    color: hover.hovered ? Qt.rgba(1, 1, 1, 0.015) : "transparent"
    Behavior on color { ColorAnimation { duration: Theme.d(150) } }

    Rectangle {
        visible: !row.first
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.stroke
    }
    RowLayout {
        id: lay
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 24
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: row.title
                color: Theme.t1
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsRow
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                visible: row.desc.length > 0
                text: row.desc
                color: row.descColor
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsText
                lineHeight: 1.5
                wrapMode: Text.Wrap
            }
        }
        RowLayout {
            id: slot
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            spacing: 8
        }
    }
    HoverHandler { id: hover }
}
