// Accueil (2026-11 mockup Main.dc.html): one sentence about what is ready, the sources that
// are set up as rows with their play button, a link to add another, and three facts about
// the pad, the picture and the screen. The dither horizon breathes along the bottom.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

VPage {
    id: root
    signal requestPage(string page)
    signal requestSetup(string source)
    dragStripHeight: 0

    Sources { id: sources }
    StreamPresets { id: presets }
    readonly property var rows: sources.rows
    readonly property string lead: rows.length > 0 ? rows[0] : ""
    readonly property bool padOn: (veyra.ps5 || {}).controller === true
    readonly property string padName: ((veyra.ps5 || {}).pad || "").toLowerCase()
    readonly property string presetLabel: { const o = presets.options.find(x => x.id === presets.current); return o ? o.label : qsTr("réglage perso") }

    function play(id) {
        if (sources.live === id) { requestPage("min"); return }
        if (sources.play(id)) requestPage("min")
        else requestSetup(id)
    }
    function sentence() {
        if (sources.live.length > 0) return qsTr("le stream continue derrière. reprends quand tu veux.")
        if (lead.length === 0) return qsTr("ajoute une ps5, une xbox, un pc ou une carte de capture. ça prend une minute.")
        const what = ({ ps5: qsTr("ta ps5 est associée"), pc: qsTr("ton pc est associé"),
                        xbox: qsTr("ta xbox est reliée à ton compte"), capture: qsTr("ta carte de capture est prête"),
                        join: qsTr("colle le lien d'un ami qui partage") })[lead]
        return what + (padOn ? qsTr(" et ta manette est branchée") : "") + qsTr(". un clic et tu es en jeu, en %1.").arg(presetLabel)
    }

    UiDither {
        id: horizon
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // The band (and its stars) starts under the last line of content.
        readonly property real contentBottom: column.y + column.height - scroller.contentY
        height: Math.max(0, Math.min(320, root.height - contentBottom - 8))
        mode: "horizon"
        opacity: 0
        NumberAnimation on opacity { id: horizonIn; running: false; to: 1; duration: 1200; easing.type: Easing.OutCubic }
    }
    // The saved PS5 is read from disk (no network), so its row shows from the first frame.
    onEnter: veyra.ps5Load()
    Component.onCompleted: { veyra.ps5Load(); horizon.opacity = 1 }
    // The staged entrance plays once, when the app opens on this page. Coming back here
    // from another page is only the page's own fade: it happens too often to stage.
    QtObject { id: launch; signal enter() }
    function intro() {
        launch.enter()
        horizon.opacity = Theme.reduced ? 1 : 0
        if (!Theme.reduced) horizonIn.restart()
    }

    Flickable {
        id: scroller
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: column.implicitHeight + column.y + 64
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: VScrollBar {}

        ColumnLayout {
            id: column
            width: Math.min(720, scroller.width - 48)
            x: Math.round((scroller.width - width) / 2)
            y: Math.round(Math.max(32, Math.min(96, (scroller.height - implicitHeight) / 3)))
            spacing: 48

            ColumnLayout {
                id: hero
                Layout.fillWidth: true
                spacing: 16
                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.lead.length > 0 ? sources.kindLabel(root.lead) + (veyra.gpuMonitorName ? " · " + veyra.gpuMonitorName.split(" ")[0].toUpperCase() : "") : ""
                    color: Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                    font.letterSpacing: 0.9
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: sources.live.length > 0 ? qsTr("tu es en jeu.") : root.lead.length > 0 ? qsTr("prêt à jouer.") : qsTr("rien à lancer.")
                    color: Theme.t1
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsHero
                    font.letterSpacing: -1.7
                    lineHeight: 1.04
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 480
                    text: root.sentence()
                    color: Theme.t2
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsLead
                    lineHeight: 1.6
                    wrapMode: Text.Wrap
                }
            }

            ColumnLayout {
                id: consoles
                Layout.fillWidth: true
                spacing: 0
                Accessible.name: qsTr("sources")
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                Repeater {
                    model: root.rows
                    delegate: Item {
                        id: row
                        required property string modelData
                        required property int index
                        readonly property bool live: sources.live === modelData
                        readonly property string st: sources.state(modelData)
                        Layout.fillWidth: true
                        implicitHeight: 80
                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: -12
                            anchors.rightMargin: -12
                            anchors.topMargin: row.index > 0 ? 1 : 0
                            radius: Theme.rMd
                            color: rowHover.hovered ? Theme.rowHover : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                        }
                        Rectangle { visible: row.index > 0; anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
                        HoverHandler { id: rowHover }
                        RowLayout {
                            anchors.fill: parent
                            spacing: 16
                            Rectangle {
                                implicitWidth: 48
                                implicitHeight: 48
                                radius: Theme.rMd
                                color: "transparent"
                                border.width: 1
                                border.color: Theme.line2
                                VIcon { anchors.centerIn: parent; name: sources.icon(row.modelData); size: 20; strokeWidth: 1.5; color: Theme.accentText }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Text {
                                    Layout.fillWidth: true
                                    text: sources.title(row.modelData)
                                    color: Theme.t1
                                    font.family: Theme.fontUi
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    UiDot { hollow: row.st === "idle" }
                                    Text {
                                        Layout.fillWidth: true
                                        text: [sources.detail(row.modelData), row.index === 0 ? root.presetLabel : ""].filter(x => x.length).join(" · ")
                                        color: Theme.t3
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fsMeta
                                        font.features: { "tnum": 1 }
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            UiButton {
                                text: qsTr("modifier")
                                kind: "ghost"
                                onClicked: root.requestSetup(row.modelData)
                            }
                            UiButton {
                                Layout.preferredWidth: 128
                                text: row.live ? qsTr("reprendre") : qsTr("jouer")
                                kind: row.index === 0 ? "solid" : "ghost"
                                size: row.index === 0 ? 48 : 40
                                iconName: "play"
                                iconFilled: true
                                iconSize: 12
                                hoverNudge: 2
                                onClicked: root.play(row.modelData)
                            }
                        }
                    }
                }
                AbstractButton {
                    id: addRow
                    Layout.fillWidth: true
                    implicitHeight: 48
                    hoverEnabled: true
                    Accessible.name: qsTr("ajouter une source")
                    onClicked: root.requestSetup("")
                    background: Item {
                        Rectangle { visible: root.rows.length > 0; anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: -12
                            anchors.rightMargin: -12
                            anchors.topMargin: 1
                            radius: Theme.rMd
                            color: addRow.hovered ? Theme.rowHover : "transparent"
                            border.width: addRow.visualFocus ? 2 : 0
                            border.color: Theme.focusRing
                            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                        }
                    }
                    contentItem: RowLayout {
                        spacing: 16
                        Item {
                            implicitWidth: 48
                            implicitHeight: 24
                            VIcon {
                                anchors.centerIn: parent
                                name: "plus"
                                size: 16
                                strokeWidth: 1.5
                                color: addRow.hovered ? Theme.t1 : Theme.t2
                                rotation: addRow.hovered ? 90 : 0
                                Behavior on rotation { NumberAnimation { duration: Theme.d(250); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.rows.length > 0 ? qsTr("ajouter une source") : qsTr("ajouter une ps5, une xbox, un pc ou une carte de capture")
                            color: addRow.hovered ? Theme.t1 : Theme.t2
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsCtl
                            elide: Text.ElideRight
                            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                        }
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            }

            GridLayout {
                id: facts
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 0
                Repeater {
                    model: [
                        { label: qsTr("MANETTE"),
                          main: root.padOn ? (root.padName || qsTr("manette branchée")) : qsTr("aucune manette"),
                          sub: root.padOn ? ((veyra.ps5 || {}).gyro === true ? qsTr("gyro · prête") : qsTr("prête")) : qsTr("branche-la en usb ou bluetooth"),
                          accent: root.padOn },
                        { label: qsTr("IMAGE"),
                          main: root.presetLabel,
                          sub: presets.summary(presets.current),
                          accent: true },
                        { label: qsTr("ÉCRAN"),
                          main: [String(Screen.model || Screen.name).trim().toLowerCase(), veyra.displayRefreshHz > 1 ? Math.round(veyra.displayRefreshHz) + " hz" : ""].filter(x => x.length).join(" · "),
                          sub: [veyra.vrrState >= 4 ? qsTr("g-sync actif") : veyra.vrrState === 3 ? qsTr("g-sync prêt") : "",
                                veyra.displayHdr > 0 ? qsTr("hdr actif") : "",
                                Math.round(Screen.width * Screen.devicePixelRatio) + "×" + Math.round(Screen.height * Screen.devicePixelRatio)].filter(x => x.length).join(" · "),
                          accent: true }
                    ]
                    delegate: Item {
                        id: fact
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: factCol.implicitHeight
                        Rectangle { visible: fact.index > 0; width: 1; height: parent.height; color: Theme.line }
                        ColumnLayout {
                            id: factCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: fact.index > 0 ? 24 : 0
                            anchors.rightMargin: 24
                            spacing: 8
                            Text { text: fact.modelData.label; color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsLabel; font.letterSpacing: 0.8 }
                            Text { Layout.fillWidth: true; text: fact.modelData.main; color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsText; elide: Text.ElideRight }
                            Text {
                                Layout.fillWidth: true
                                text: fact.modelData.sub
                                color: fact.modelData.accent ? Theme.accentText : Theme.t3
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsMeta
                                font.features: { "tnum": 1 }
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.bottomMargin: 12
        width: credits.implicitWidth + 16
        height: credits.implicitHeight + 8
        radius: Theme.rMd
        color: Qt.rgba(Theme.bgOuter.r, Theme.bgOuter.g, Theme.bgOuter.b, 0.85)
        Text {
            id: credits
            anchors.centerIn: parent
            text: qsTr("BooVI · gpl-3.0 · basé sur veyra et chiaki-ng")
            color: Theme.t2
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsLabel
        }
    }

    UiEnter { target: hero; order: 0; page: launch }
    UiEnter { target: consoles; order: 1; page: launch }
    UiEnter { target: facts; order: 2; page: launch }
}
