// Fork: what the PS5 receives from this PC, shown over the picture while a PS5 stream runs.
// Upstream paused the controller silently whenever no Veyra window had the focus; the
// card says so. The chips name the pad being read, view-only mode, and the keyboard/mouse.
// Both parts are cut out of the native video window (videoCover), like the dialogs.
import QtQuick
import QtQuick.Controls

Item {
    id: chips
    objectName: "ps5-input-chips"
    readonly property var ps5: veyra.ps5 || ({})
    readonly property bool streaming: ps5.active === true
    readonly property bool viewOnly: ps5.viewOnly === true
    readonly property bool paused: streaming && ps5.inputPaused === true
    readonly property bool kbm: ps5.kbm === true
    readonly property string padName: ps5.pad || ""
    readonly property var ease: [0.2, 0, 0, 1, 1, 1]
    // Bottom inset of the controls that can sit under the card (cinema pill, fullscreen bar).
    property real bottomInset: 0

    Rectangle {
        id: card
        readonly property bool videoCover: true
        property real coverRadius: 8
        objectName: "ps5-paused-card"
        width: 432
        height: cardCol.implicitHeight + 48
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -chips.bottomInset / 2
        radius: 8
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke2
        visible: opacity > 0.01
        opacity: chips.paused ? 1 : 0
        scale: chips.paused ? 1 : 0.96
        Behavior on opacity { NumberAnimation { duration: Theme.d(chips.paused ? 220 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: chips.ease } }
        Behavior on scale { NumberAnimation { duration: Theme.d(chips.paused ? 260 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: chips.ease } }

        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.exp }
        Column {
            id: cardCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 24 }
            spacing: 16
            Row {
                spacing: 12
                Rectangle { width: 4; height: 14; radius: 1; color: Theme.exp; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: qsTr("手柄已暂停")
                    color: Theme.t1
                    font.family: Theme.fontMono
                    font.pixelSize: 15
                }
            }
            Text {
                width: parent.width
                text: qsTr("BooVI 不在前台，PS5 收不到你的按键。点击画面即可继续。")
                wrapMode: Text.Wrap
                color: Theme.t2
                font.family: Theme.fontUi
                font.pixelSize: 14
                lineHeight: 1.5
            }
            VButton {
                text: qsTr("继续")
                primary: true
                // The click itself brings the window back to the foreground; this covers a keyboard press.
                onClicked: Window.window.requestActivate()
            }
        }
    }

    Row {
        id: row
        anchors { right: parent.right; top: parent.top; rightMargin: 24; topMargin: 24 }
        spacing: 8
        visible: opacity > 0.01
        opacity: chips.streaming ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: chips.ease } }

        Rectangle {
            readonly property bool videoCover: true
            property real coverRadius: 4
            height: 32
            width: padText.implicitWidth + 24
            radius: 4
            color: Theme.card
            border.width: 1
            border.color: chips.viewOnly ? Theme.warn : Theme.stroke2
            Text {
                id: padText
                anchors.centerIn: parent
                text: chips.viewOnly ? qsTr("仅观看") : (chips.padName.length > 0 ? chips.padName : qsTr("未检测到电脑手柄"))
                color: chips.viewOnly ? Theme.warn : (chips.padName.length > 0 ? Theme.t1 : Theme.t3)
                font.family: Theme.fontMono
                font.pixelSize: 11
            }
        }

        AbstractButton {
            id: kbmButton
            readonly property bool videoCover: true
            property real coverRadius: 4
            visible: !chips.viewOnly
            height: 32
            width: kbmText.implicitWidth + 24
            hoverEnabled: true
            Accessible.name: kbmText.text
            onClicked: veyra.ps5Kbm(!chips.kbm)
            scale: pressed ? 0.96 : 1
            Behavior on scale { NumberAnimation { duration: Theme.d(150); easing.type: Easing.BezierSpline; easing.bezierCurve: chips.ease } }
            background: Rectangle {
                radius: 4
                color: chips.kbm ? Theme.expSoft : (kbmButton.hovered ? Theme.card2 : Theme.card)
                border.width: 1
                border.color: chips.kbm ? Theme.exp : Theme.stroke2
                Behavior on color { ColorAnimation { duration: Theme.d(150) } }
            }
            contentItem: Text {
                id: kbmText
                text: chips.kbm ? qsTr("键盘鼠标已接管 · Ctrl+Alt+Shift+Z 释放") : qsTr("键盘鼠标")
                color: chips.kbm ? Theme.exp : Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
