// Fork: the PS5 stream's word on the PC's pad, under the pad chip (StreamOverlay.qml).
// A DualSense still paired with the PS5 never reaches the PC: after 3 s of a live stream
// without a pad the hint says how to bring it over, and goes as soon as a pad is read
// (PadPresence.h). A pad that comes in mid-stream gets a short toast with its name.
import QtQuick
import QtQuick.Layouts

Column {
    id: notice
    property bool hint: false
    property string padName: ""
    property int arrivals: 0      // veyra.ps5.padArrivals: each increase shows the toast
    signal closeRequested()
    spacing: 8
    visible: card.visible || toast.visible   // takes no room in the chip column while empty

    property int arrivalsSeen: 0
    Component.onCompleted: arrivalsSeen = arrivals
    onArrivalsChanged: {
        if (arrivals > arrivalsSeen) { toast.on = true; toastTimer.restart() }
        arrivalsSeen = arrivals
    }

    Rectangle {
        id: card
        objectName: "pad-hint"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        width: 360
        height: cardCol.implicitHeight + 32
        radius: Theme.rLg
        color: Theme.popover
        border.width: 1
        border.color: Theme.stroke2
        visible: opacity > 0.01
        opacity: notice.hint ? 1 : 0
        transform: Translate { y: notice.hint || Theme.reduced ? 0 : -4; Behavior on y { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(notice.hint ? 220 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: title.text
        ColumnLayout {
            id: cardCol
            x: 16
            y: 16
            width: parent.width - 32
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                VIcon { name: "pad"; size: 14; strokeWidth: 1.5; color: Theme.t2 }
                Text { id: title; Layout.fillWidth: true; text: qsTr("aucune manette sur ce pc"); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsRow; elide: Text.ElideRight }
                UiButton { kind: "tool"; size: 24; iconSize: 12; iconName: "winclose"; tip: qsTr("fermer"); onClicked: notice.closeRequested() }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("elle est peut-être encore reliée à la ps5. branche-la en usb, ou éteins-la, maintiens create puis ps jusqu'à ce que la barre lumineuse clignote, et ajoute-la dans le bluetooth de windows.")
                color: Theme.t2
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsRow
                lineHeight: 1.5
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("le clavier et la souris marchent aussi.")
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                wrapMode: Text.Wrap
            }
        }
    }

    Rectangle {
        id: toast
        objectName: "pad-arrived"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rMd
        property bool on: false
        readonly property bool shown: on && notice.padName.length > 0 && !notice.hint
        width: toastRow.implicitWidth + 24
        height: 32
        radius: Theme.rMd
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        visible: opacity > 0.01
        opacity: shown ? 1 : 0
        transform: Translate { y: toast.shown || Theme.reduced ? 0 : -4; Behavior on y { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(toast.shown ? 220 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        Accessible.role: Accessible.StaticText
        Accessible.name: toastText.text
        Timer { id: toastTimer; interval: 2600; onTriggered: toast.on = false }
        Row {
            id: toastRow
            anchors.centerIn: parent
            spacing: 8
            VIcon { anchors.verticalCenter: parent.verticalCenter; name: "pad"; size: 14; strokeWidth: 1.5; color: Theme.accentText }
            Text {
                id: toastText
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("manette connectée · %1").arg(notice.padName.toLowerCase())
                color: Theme.t1
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
            }
        }
    }
}
