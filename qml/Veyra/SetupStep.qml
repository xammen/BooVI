// One step of a source's setup: its heading, its sentence, then its controls. Steps swap
// in with a short staggered rise (320 ms, 60 ms apart) each time they become current.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: step
    property string title: ""
    property string text: ""
    property bool current: false
    default property alias content: body.data
    signal enter()
    visible: current
    spacing: 32
    Layout.fillWidth: true
    onCurrentChanged: if (current) enter()

    ColumnLayout {
        id: head
        Layout.fillWidth: true
        spacing: 12
        Text {
            Layout.fillWidth: true
            text: step.title
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsTitle
            font.letterSpacing: -1
            lineHeight: 1.1
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: step.text.length > 0
            text: step.text
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsLead
            lineHeight: 1.6
            wrapMode: Text.Wrap
        }
    }
    ColumnLayout {
        id: body
        Layout.fillWidth: true
        spacing: 24
    }
    UiEnter { target: head; page: step; order: 0; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: body; page: step; order: 1; stagger: 60; dy: 6; duration: 320 }
}
