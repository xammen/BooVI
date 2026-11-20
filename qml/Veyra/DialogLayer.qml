// Shared dialog presentation; extracted without changing geometry or motion.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: dlg
    required property Item dialogHost
    property string title: ""
    property string sub: ""
    property string glyph: ""
    property int dialogWidth: 640
    property var actions: []
    // dialogs.js `foot`: a quiet note at the left of the button row.
    property string foot: ""
    default property alias body: bodyCol.data
    signal actionTriggered(string label)

    // Which dialog key shows this layer.
    property string key: ""
    readonly property bool shown: key.length > 0 && dialogHost.dialog === key
    // Arrives from 0.975 and 14px low over 420 ms, settling without overshoot; leaves faster
    // (180 ms, accelerating) to 0.985 and 8px, a smaller move than its entrance (La Cuisine).
    property real motionS: 0.975
    property real motionDy: 14
    onShownChanged: { if (shown) { exitAnim.stop(); motionS = 0.975; motionDy = 14; enterAnim.restart() } else { enterAnim.stop(); exitAnim.restart() } }
    onMotionSChanged: if (shown) dialogHost.motionScale = motionS
    ParallelAnimation {
        id: enterAnim
        NumberAnimation { target: dlg; property: "motionS"; to: 1; duration: Theme.d(Theme.durEnter); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
        NumberAnimation { target: dlg; property: "motionDy"; to: 0; duration: Theme.d(Theme.durEnter); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
    }
    ParallelAnimation {
        id: exitAnim
        NumberAnimation { target: dlg; property: "motionS"; to: 0.985; duration: Theme.d(Theme.durExit); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeExit }
        NumberAnimation { target: dlg; property: "motionDy"; to: 8; duration: Theme.d(Theme.durExit); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeExit }
    }
    opacity: shown ? 1 : 0
    visible: opacity > 0
    // A closed dialog leaves the scene. Its live bindings (capture preview, fps text)
    // kept changing while hidden and Qt redrew the window for each change, ~40 times
    // a second during capture; OBS game capture then held the UI instead of the video.
    // Keyed on shown/opacity, never on visible: an item without a parent always
    // reads invisible, so "visible ? host : null" never brought a dialog back.
    parent: (shown || opacity > 0) ? dialogHost : null
    Behavior on opacity { NumberAnimation { duration: Theme.d(dlg.shown ? 280 : Theme.durExit) } }
    transform: [
        Scale { origin.x: dlg.width / 2; origin.y: dlg.height / 2; xScale: dlg.motionS; yScale: dlg.motionS },
        Translate { y: dlg.motionDy }
    ]
    // Cut out of the native video window (main.cpp syncVideoCovers). The flag,
    // not the objectName: each dialog below names itself for tests.
    objectName: "videoCover"
    readonly property bool videoCover: true
    property real coverRadius: 16

    // Centred by position, not anchors: the parent is null while closed.
    x: Math.round((dialogHost.width - width) / 2)
    y: Math.round((dialogHost.height - height) / 2)
    width: dialogWidth
    // Never taller than the window: the body scrolls instead of the panel
    // being cut off at the top and bottom of a small window.
    // Header and footer each carry 16px margins top and bottom (64 in all); the
    // old +40 left every body 24px short, so short dialogs hid their last row.
    implicitHeight: Math.min(720, dialogHost.height - 24, header.height + bodyScroll.contentHeight + footer.height + 64)
    height: implicitHeight
    // Swallow presses on the panel itself so they never reach the scrim
    // below (a passive tap on a control used to close the whole dialog).
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; z: -1 }
    radius: 16
    color: Theme.dialog
    border.width: 1
    border.color: Theme.stroke2

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            id: header
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 12
            Rectangle {
                implicitWidth: 34; implicitHeight: 34; radius: 10
                color: Qt.rgba(1, 1, 1, 0.06)
                VIcon { anchors.centerIn: parent; name: dlg.glyph }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    // Fill: without a subtitle this is the column's only child, and
                    // its natural width would cap the column and pull the close
                    // button in beside the title.
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: dlg.title
                    color: Theme.t1
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsH2
                    font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: dlg.sub.length > 0
                    text: dlg.sub
                    color: Theme.t3
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
            VButton { icon: true; iconName: "x"; ghost: true; onClicked: dialogHost.close() }
        }

        Flickable {
            id: bodyScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            contentHeight: bodyCol.implicitHeight
            clip: true
            ScrollBar.vertical: VScrollBar { }
            ColumnLayout {
                id: bodyCol
                // Keep the groups clear of the overlaid scroll bar.
                width: parent.width - (bodyScroll.contentHeight > bodyScroll.height ? 12 : 0)
                spacing: 0
            }
        }

        RowLayout {
            id: footer
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 8
            Text {
                objectName: "dialog-foot"
                Layout.fillWidth: true
                visible: dlg.foot.length > 0
                text: dlg.foot
                color: Theme.t3
                font.family: Theme.fontUi
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true; visible: dlg.foot.length === 0 }
            Repeater {
                model: dlg.actions
                delegate: VButton {
                    required property var modelData
                    text: modelData.label
                    iconName: modelData.icon || ""
                    primary: modelData.primary === true
                    onClicked: dlg.actionTriggered(modelData.label)
                }
            }
        }
    }
}
