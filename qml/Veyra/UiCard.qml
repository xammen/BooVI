// A settings surface: card colour, 8px radius, a 1px ring for its edge. Rows go inside.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: card
    default property alias content: col.data
    Layout.fillWidth: true
    implicitHeight: col.implicitHeight
    radius: Theme.rLg
    color: Theme.card
    border.width: 1
    border.color: Theme.stroke
    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0
    }
}
