// A text field: mono, 44px, ring that lights in the accent on focus. `edited` fires once
// per committed value (Enter or focus leaving), not per keystroke.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

Basic.TextField {
    id: f
    property int fieldHeight: 44
    property real textSize: Theme.fsText
    signal edited(string text)
    implicitHeight: fieldHeight
    implicitWidth: 256
    leftPadding: 12
    rightPadding: 12
    color: Theme.t1
    placeholderTextColor: Qt.rgba(Theme.t3.r, Theme.t3.g, Theme.t3.b, 0.75)
    selectionColor: Theme.accentFill
    selectedTextColor: Theme.t1
    font.family: Theme.fontMono
    font.pixelSize: textSize
    verticalAlignment: TextInput.AlignVCenter
    onEditingFinished: edited(text)
    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: Theme.rMd + 4
            color: "transparent"
            border.width: 4
            border.color: Theme.accentSoft
            opacity: f.activeFocus ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.d(150) } }
        }
        Rectangle {
            anchors.fill: parent
            radius: Theme.rMd
            color: Theme.fieldBg
            border.width: 1
            border.color: f.activeFocus ? Theme.accent : hover.hovered ? Qt.lighter(Theme.line2, 1.25) : Theme.line2
            Behavior on border.color { ColorAnimation { duration: Theme.d(150) } }
        }
    }
    HoverHandler { id: hover; cursorShape: Qt.IBeamCursor }
}
