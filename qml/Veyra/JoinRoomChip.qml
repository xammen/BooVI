// Rejoindre un partage (fork): on a friend's share joined from this BooVI, the host's room
// (RoomModel.h, GuestJoinModel state.room) stays on the picture, top centre: who holds the
// controller, this PC's place, and the one action that fits (take, wait, leave the queue, give
// back), plus the dice when the host allows them. The host decides everything; this only asks,
// and says for a few seconds why the host refused (room.notice).
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: chip
    property var room: ({})
    readonly property bool videoCover: true
    property real coverRadius: Theme.rLg
    readonly property bool shown: room.you !== undefined
    readonly property var members: room.members || []
    function nameOf(id) { for (const m of members) if (m.id === id) return m.name; return "" }
    readonly property string role: room.role || ""
    readonly property int place: (room.queue || []).indexOf(room.you) + 1
    readonly property string holder: room.holder ? nameOf(room.holder) : ""
    readonly property int players: members.filter(m => m.seat === "player").length
    readonly property string refusal: room.notice || ""
    property string notice: ""
    onRefusalChanged: if (refusal) { notice = refusalText(refusal); noticeTimer.restart() }
    function refusalText(why) {
        return why === "watcher" ? qsTr("l'hôte t'a mis en spectateur")
             : why === "not-allowed" ? qsTr("l'hôte a coupé les dés")
             : why === "too-soon" ? qsTr("les dés viennent d'être lancés, attends un peu")
             : why === "too-few" ? qsTr("il faut au moins deux joueurs pour les dés")
             : why === "busy" ? qsTr("les dés roulent déjà") : qsTr("l'hôte a refusé")
    }
    Timer { id: noticeTimer; interval: 3200; onTriggered: chip.notice = "" }
    width: row.implicitWidth + 16
    height: 36
    radius: Theme.rLg
    color: Theme.popover
    border.width: 1
    border.color: Theme.stroke2
    visible: opacity > 0.01
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.d(220); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    Accessible.role: Accessible.Pane
    Accessible.name: label.text
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8
        UiDot { hollow: chip.role !== "hold" }
        Text {
            id: label
            text: chip.notice ? chip.notice
                : chip.role === "hold" ? qsTr("tu as la manette")
                : chip.role === "queue" ? qsTr("file n° %1").arg(chip.place)
                : chip.role === "watch" ? qsTr("tu regardes")
                : chip.room.owner ? qsTr("l'hôte joue")
                : chip.holder ? qsTr("%1 joue").arg(chip.holder) : qsTr("manette libre")
            color: chip.notice ? Theme.warn : Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
        }
        UiButton {
            visible: chip.role !== "watch"
            size: 26
            text: chip.role === "hold" ? qsTr("rendre") : chip.role === "queue" ? qsTr("quitter la file") : chip.holder || chip.room.owner ? qsTr("attendre") : qsTr("prendre")
            onClicked: veyra.join.room(chip.role === "hold" ? "release" : chip.role === "queue" ? "cancel" : "ask")
        }
        UiButton {
            visible: chip.room.allowDice === 1 && chip.players >= 2 && chip.role !== "watch"
            size: 26
            iconName: "sparkles"
            tip: qsTr("lancer les dés pour la manette")
            onClicked: veyra.join.room("roll")
        }
    }
}
