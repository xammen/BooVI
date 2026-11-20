// A status line under a flow: three breathing dots while something runs, then the text.
import QtQuick
import QtQuick.Layouts

RowLayout {
    id: note
    property string text: ""
    property bool busy: false
    property color color: Theme.t3
    visible: text.length > 0 || busy
    spacing: 12
    Layout.fillWidth: true
    Row {
        visible: note.busy
        spacing: 4
        Layout.alignment: Qt.AlignVCenter
        Repeater {
            model: 3
            delegate: Rectangle {
                required property int index
                width: 4
                height: 4
                radius: 2
                color: Theme.t3
                opacity: 0.25
                SequentialAnimation on opacity {
                    running: note.busy && note.visible && !Theme.reduced
                    loops: Animation.Infinite
                    PauseAnimation { duration: index * 150 }
                    NumberAnimation { to: 1; duration: 480; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 0.25; duration: 720 - index * 150; easing.type: Easing.InOutSine }
                    PauseAnimation { duration: index * 150 }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        text: note.text
        color: note.color
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsMeta
        wrapMode: Text.Wrap
    }
}
