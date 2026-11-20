// The shell's top bar, shared by accueil, consoles and réglages: the blinking face, the
// name and the version on the leading edge, the three destinations and the window buttons
// on the trailing edge. A line under the current destination slides to the next one. The
// bar itself moves the window, and a double click maximises it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: bar
    property string current: "home"
    signal navigate(string page)
    implicitHeight: 48
    readonly property var shell: Window.window

    DragHandler {
        target: null
        enabled: bar.shell && !bar.shell.fullscreen && !bar.shell.maximized
        onActiveChanged: if (active) bar.shell.startSystemMove()
    }
    TapHandler { onDoubleTapped: bar.shell.toggleMaximized() }

    // The consoles page's dotted band behind the bar.
    UiDither {
        anchors.fill: parent
        cell: 2
        mode: "band"
        opacity: bar.current === "consoles" ? 0.5 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 12
        spacing: 12
        UiFace { Layout.preferredWidth: 96 }
        Text {
            text: "BooVI"
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsCtl
            font.weight: Font.Medium
        }
        Rectangle {
            implicitWidth: pill.implicitWidth + 16
            implicitHeight: pill.implicitHeight + 4
            radius: height / 2
            color: "transparent"
            border.width: 1
            border.color: Theme.line2
            Text {
                id: pill
                anchors.centerIn: parent
                text: veyra.version
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsLabel
                font.letterSpacing: 0.6
            }
        }
        Item { Layout.fillWidth: true }
        Item {
            id: tabs
            Layout.leftMargin: 12
            Layout.fillHeight: true
            implicitWidth: tabRow.implicitWidth
            property Item active: null
            Row {
                id: tabRow
                height: parent.height
                spacing: 24
                Repeater {
                    model: [{ id: "home", label: qsTr("accueil") }, { id: "consoles", label: qsTr("consoles") }, { id: "set", label: qsTr("réglages") }]
                    delegate: AbstractButton {
                        id: link
                        required property var modelData
                        readonly property bool here: bar.current === modelData.id
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 32
                        implicitWidth: linkText.implicitWidth
                        hoverEnabled: true
                        Accessible.name: modelData.label
                        onClicked: bar.navigate(modelData.id)
                        background: Item { UiFocusRing { innerRadius: Theme.rMd; gap: 4; visible: link.visualFocus } }
                        contentItem: Text {
                            id: linkText
                            text: link.modelData.label
                            verticalAlignment: Text.AlignVCenter
                            color: link.here || link.hovered ? Theme.t1 : Theme.t2
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsCtl
                            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        onHereChanged: if (here) tabs.active = link
                        Component.onCompleted: if (here) tabs.active = link
                    }
                }
            }
            // Sits on the bar's bottom line under the current destination. It only slides
            // once the row has its first layout, so it does not fly in at launch.
            Rectangle {
                id: underline
                property bool armed: false
                x: tabs.active ? tabs.active.x : 0
                width: tabs.active ? tabs.active.width : 0
                y: tabs.height - height
                height: 2
                radius: 1
                color: Theme.t1
                Timer { running: true; interval: 400; onTriggered: underline.armed = true }
                Behavior on x { enabled: underline.armed; NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                Behavior on width { enabled: underline.armed; NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
            }
        }
        Rectangle { Layout.leftMargin: 12; implicitWidth: 1; implicitHeight: 20; color: Theme.line }
        UiWindowButtons {}
    }
}
