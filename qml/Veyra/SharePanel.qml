// Partager (fork): the stream toolbar's panel for the remote guests (GuestShareModel, `vyGuest`).
// Start sharing, copy or renew the link, see who watches and how, pass the controller, set who
// may play, roll the dice, kick one viewer. Through BooVI's broker the room opens with the share
// by itself; in the tailnet, `tailscale serve` only runs from the button here.
// `preview` shows sample values without a session (--page stream-preview --share-panel).
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: panel
    property bool open: false
    property bool preview: false
    property real maxHeight: 640
    signal closeRequested()
    readonly property bool videoCover: true
    property real coverRadius: Theme.rLg
    readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
    readonly property bool active: preview || (g !== null && g.active)
    readonly property string phase: preview ? "connected" : g ? g.phase : "off"
    readonly property var viewers: preview ? [
            { tag: "a1", name: "lucas", connected: true, seat: "player", holder: true, queue: 0, client: "browser", path: qsTr("direct"), rtt: 18.4, mbps: 24.1, fps: 60,
              feed: "real", size: "native", auto: true, kbps: 25000, share: 25000, height: 1440, link: 85000, linkRtt: 6.2, hz: 144, guestFps: 59.8,
              pad: true, padName: "DualSense", padMapping: "standard", presentToDisplay: 14.2, inputToPs5: 9.8, access: "lucas" },
            { tag: "b2", name: "zoé", connected: true, seat: "player", holder: false, queue: 1, client: "boovi", path: qsTr("direct"), rtt: 22.0, mbps: 8.2, fps: 60,
              feed: "real", size: "1080", auto: true, kbps: 8333, share: 8333, height: 1080, pad: true, padName: "DualSense" },
            { tag: "c3", name: "sam", connected: true, seat: "watcher", holder: false, queue: 0, client: "browser", path: qsTr("relais cloudflare"), rtt: 41.5, mbps: 8.1, fps: 120,
              feed: "120", size: "1080", auto: true, kbps: 8333, share: 8333, height: 1080, pad: false }]
        : g ? g.viewers : []
    readonly property var room: preview ? ({ holder: "a1", holderName: "lucas", held: 734, owner: false, queue: ["zoé"], turn: 30, diceAllowed: true, max: 4, count: 3, upload: 50000,
                                              dice: { n: 1, winner: "lucas", age: 120, by: "zoé", rolls: [{ name: "lucas", value: 6 }, { name: "zoé", value: 3 }] } })
                                       : g ? g.room : ({})
    readonly property int players: viewers.filter(v => v.connected && v.seat === "player").length
    property string selected: ""
    readonly property var sel: { for (const v of viewers) if (v.tag === selected) return v; return null }
    // The host's maximum (Réglages → partage), changed here during a share too.
    readonly property string maxFeed: typeof veyra !== "undefined" && veyra.preferences.guestFeed ? veyra.preferences.guestFeed : "120"
    readonly property string maxSize: typeof veyra !== "undefined" && veyra.preferences.guestResolution ? veyra.preferences.guestResolution : "native"
    function feedText(f) { return f === "120" ? qsTr("fluide 120") : f === "60" ? qsTr("fluide 60") : qsTr("images réelles") }
    function mbps(kbps) { return kbps > 0 ? Number(kbps / 1000).toFixed(kbps < 10000 ? 1 : 0).replace(".", ",") + " Mb/s" : "—" }
    function ms(v) { return v === undefined || v === null || v < 0 ? "—" : Number(v).toFixed(1).replace(".", ",") + " ms" }
    function minutes(s) { return s < 60 ? qsTr("%1 s").arg(Math.round(s)) : qsTr("%1 min").arg(Math.floor(s / 60)) }
    function role(v) {
        if (!v.connected) return qsTr("se connecte…")
        if (v.holder) return qsTr("a la manette")
        if (v.queue > 0) return qsTr("file n° %1").arg(v.queue)
        return v.seat === "watcher" ? qsTr("regarde seulement") : qsTr("regarde")
    }
    function receives(st) {
        return !st.feed ? "" : (st.auto ? qsTr("auto") : qsTr("manuel")) + " · " + feedText(st.feed) + " · " + (st.height ? st.height + "p" : "") + " · " + mbps(st.kbps)
            + (st.guestFps !== undefined && st.guestFps !== null ? " · " + Math.round(st.guestFps) + " i/s" : "")
    }
    readonly property string reach: preview ? "broker" : g ? g.reach : "local"
    readonly property bool viaBroker: reach === "broker"
    readonly property string broker: preview ? "connected" : g ? g.broker : "off"
    readonly property string brokerHost: preview ? "go.example.com" : g ? g.brokerHost : ""
    // Test grabs (--grab-share) never carry the link's secret.
    readonly property bool masked: typeof vyTest !== "undefined" && (vyTest.grabShare || "").length > 0
    readonly property string link: preview ? "https://go.example.com/••••••#t=••••••••" : !g ? "" : masked ? g.link.replace(/#.*/, "#t=••••••••") : g.link
    readonly property string mode: preview ? "guest" : g ? g.mode : "guest"
    readonly property real sent: preview ? 40.4 : g ? g.sentMbps : 0

    width: 420
    height: Math.min(maxHeight, col.implicitHeight + 40 + 1)
    radius: Theme.rLg
    color: Theme.popover
    border.width: 1
    border.color: Theme.stroke2
    clip: true
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    transformOrigin: Item.Bottom
    scale: open || Theme.reduced ? 1 : 0.98
    transform: Translate {
        y: panel.open || Theme.reduced ? 0 : 10
        Behavior on y { NumberAnimation { duration: Theme.d(panel.open ? 260 : 180); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    }
    Behavior on opacity { NumberAnimation { duration: Theme.d(panel.open ? 220 : 160); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    Behavior on scale { NumberAnimation { duration: Theme.d(panel.open ? 260 : 180); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("partager le stream")
    Rectangle { x: parent.radius; y: 1; width: parent.width - 2 * parent.radius; height: 1; color: Theme.lit }

    component Meta: Text {
        Layout.fillWidth: true
        color: Theme.t2
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsMeta
        lineHeight: 1.4
        wrapMode: Text.Wrap
    }
    component Label: Text { color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsLabel }

    Flickable {
        anchors.fill: parent
        anchors.topMargin: 21
        anchors.bottomMargin: 20
        contentHeight: col.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

    ColumnLayout {
        id: col
        x: 20
        width: panel.width - 40
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            VIcon { name: "users"; size: 14; strokeWidth: 1.5; color: Theme.accentText }
            Text { text: qsTr("partager le stream"); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsRow }
            Item { Layout.fillWidth: true }
            UiButton { kind: "tool"; size: 24; iconSize: 12; iconName: "winclose"; tip: qsTr("fermer"); onClicked: panel.closeRequested() }
        }

        // --- off -------------------------------------------------------------------------
        Meta {
            visible: !panel.active
            text: qsTr("tes amis regardent ton stream depuis leur navigateur ou BooVI, avec le son ; un seul tient la manette à la fois. il leur faut le lien, chrome ou edge, et une manette pour jouer.")
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsText
            lineHeight: 1.5
        }
        UiButton {
            visible: !panel.active
            kind: "solid"
            size: 36
            text: qsTr("démarrer le partage")
            iconName: "users"
            onClicked: if (panel.g) panel.g.start()
        }
        Meta { visible: panel.g !== null && panel.g.error.length > 0; text: panel.g ? panel.g.error : ""; color: Theme.err }

        // --- the link --------------------------------------------------------------------
        RowLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.fillWidth: true
                height: 32
                radius: Theme.rMd
                color: Theme.fieldBg
                border.width: 1
                border.color: Theme.line
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    text: panel.link.length > 0 ? panel.link : qsTr("ouverture du salon…")
                    color: panel.link.length > 0 ? Theme.accentText : Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                    elide: Text.ElideMiddle
                }
            }
            UiButton { size: 32; text: qsTr("copier"); iconName: "copy"; enabled: panel.link.length > 0; onClicked: if (panel.g) panel.g.copyLink() }
            UiButton { size: 32; iconName: "refresh"; tip: qsTr("nouveau lien : ceux qui ont l'ancien partent"); enabled: panel.link.length > 0; onClicked: if (panel.g && !panel.preview) panel.g.renewLink() }
        }
        Meta {
            visible: panel.active
            text: panel.viaBroker ? (panel.broker === "connected" ? qsTr("salon ouvert sur %1 · ").arg(panel.brokerHost) : qsTr("ouverture du salon… · "))
                                    + qsTr("tes amis ouvrent ce lien dans chrome ou edge, sans rien installer.")
                : panel.reach === "tailnet-https" ? qsTr("lien https de ton tailnet : tes amis doivent y être, ou tu leur partages ce pc dans tailscale.")
                : panel.reach === "tailnet-http" ? qsTr("lien http de ton tailnet : l'image passe, mais les navigateurs ne lisent la manette qu'en https.")
                : qsTr("tailscale est coupé : seul ce pc peut ouvrir ce lien.")
            color: panel.reach === "tailnet-http" || panel.reach === "local" ? Theme.warn : Theme.t3
        }
        Flow {
            visible: panel.active && !panel.preview && !panel.viaBroker
            Layout.fillWidth: true
            spacing: 8
            UiButton {
                visible: panel.reach === "tailnet-http"
                size: 32; text: qsTr("activer https (tailscale serve)"); iconName: "shield"
                enabled: panel.g !== null && !panel.g.busy
                onClicked: panel.g.enableHttps()
            }
            UiButton {
                visible: panel.reach === "tailnet-https"
                size: 32; text: qsTr("couper https")
                enabled: panel.g !== null && !panel.g.busy
                onClicked: panel.g.disableHttps()
            }
        }

        // --- the controller ------------------------------------------------------------------
        ColumnLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("la manette") }
                Item { Layout.fillWidth: true }
                Label {
                    text: panel.viewers.length > 0 ? qsTr("%1/%2 invités · envoi %3").arg(panel.viewers.length).arg(panel.room.max || 4).arg(Number(panel.sent).toFixed(1).replace(".", ",") + " Mb/s")
                                                   : qsTr("en attente d'invités")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                UiDot { hollow: !panel.room.holder && !panel.room.owner }
                Text {
                    Layout.fillWidth: true
                    text: panel.room.owner ? qsTr("tu as repris la manette")
                        : panel.room.holder ? qsTr("%1 joue depuis %2").arg(panel.room.holderName).arg(panel.minutes(panel.room.held || 0))
                        : panel.viewers.length > 0 ? qsTr("manette libre : le premier qui la demande l'a") : qsTr("personne ne joue")
                    color: Theme.t1
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsText
                    elide: Text.ElideRight
                }
            }
            Meta {
                visible: (panel.room.queue || []).length > 0
                text: qsTr("file : %1").arg((panel.room.queue || []).join(", "))
            }
            Meta {
                readonly property var d: panel.room.dice || ({})
                visible: d.winner !== undefined && d.age < 600
                text: d.rolls ? qsTr("dés : %1 gagne (%2)").arg(d.winner).arg(d.rolls.map(r => r.name + " " + r.value).join(", ")) : ""
                color: Theme.accentText
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                UiButton { visible: panel.room.holder !== undefined && panel.room.holder.length > 0 && !panel.room.owner; size: 32; text: qsTr("reprendre la manette"); iconName: "gamepad"
                           onClicked: if (panel.g && !panel.preview) panel.g.takeBack() }
                UiButton { visible: panel.room.owner === true; size: 32; text: qsTr("rendre la manette"); iconName: "gamepad"
                           onClicked: if (panel.g && !panel.preview) panel.g.handBack() }
                UiButton { visible: panel.players >= 2; size: 32; text: qsTr("lancer les dés"); iconName: "sparkles"
                           onClicked: if (panel.g && !panel.preview) panel.g.roll() }
            }
            Meta {
                visible: panel.mode === "watch" && panel.viewers.length > 0
                text: qsTr("regarder seulement : aucune manette d'invité n'arrive à la ps5.")
                color: Theme.warn
            }
        }

        // --- who watches ---------------------------------------------------------------------
        ColumnLayout {
            visible: panel.active && panel.viewers.length > 0
            Layout.fillWidth: true
            spacing: 2
            Repeater {
                model: panel.viewers
                delegate: Rectangle {
                    id: rowItem
                    required property var modelData
                    readonly property bool chosen: panel.selected === modelData.tag
                    Layout.fillWidth: true
                    implicitHeight: 44
                    radius: Theme.rMd
                    color: chosen ? Theme.fieldBg : hover.hovered ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.d(120) } }
                    HoverHandler { id: hover }
                    TapHandler { onTapped: panel.selected = rowItem.chosen ? "" : rowItem.modelData.tag }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 4
                        spacing: 10
                        UiDot { hollow: !rowItem.modelData.connected }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: rowItem.modelData.name + (rowItem.modelData.client === "boovi" ? " · BooVI" : "")
                                color: Theme.t1
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsText
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: panel.role(rowItem.modelData) + (rowItem.modelData.path ? " · " + rowItem.modelData.path : "") + " · " + panel.ms(rowItem.modelData.rtt)
                                color: rowItem.modelData.holder ? Theme.accentText : Theme.t3
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsLabel
                                elide: Text.ElideRight
                            }
                        }
                        UiButton { kind: "tool"; size: 28; iconSize: 14; iconName: "gamepad"; tip: qsTr("lui passer la manette")
                                   visible: rowItem.modelData.connected && !rowItem.modelData.holder
                                   onClicked: if (panel.g && !panel.preview) panel.g.give(rowItem.modelData.tag) }
                        UiButton { kind: "tool"; size: 28; iconSize: 14; iconName: rowItem.modelData.seat === "watcher" ? "eyeoff" : "eye"
                                   tip: rowItem.modelData.seat === "watcher" ? qsTr("regarde seulement · cliquer : peut jouer") : qsTr("peut jouer · cliquer : regarde seulement")
                                   active: rowItem.modelData.seat === "watcher"
                                   onClicked: if (panel.g && !panel.preview) panel.g.setSeat(rowItem.modelData.tag, rowItem.modelData.seat === "watcher" ? "player" : "watcher") }
                        UiButton { kind: "tool"; size: 28; iconSize: 14; iconName: "x"; inkOverride: Theme.err
                                   tip: rowItem.modelData.access ? qsTr("couper sa visite") : qsTr("le déconnecter")
                                   onClicked: if (panel.g && !panel.preview) panel.g.kick(rowItem.modelData.tag) }
                    }
                }
            }
        }

        // --- the selected viewer -------------------------------------------------------------
        ColumnLayout {
            id: detail
            visible: panel.active && panel.sel !== null
            Layout.fillWidth: true
            spacing: 6
            readonly property var st: panel.sel || ({})
            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: 16
                rowSpacing: 6
                Repeater {
                    model: [
                        { k: qsTr("ping"), v: panel.ms(detail.st.rtt) },
                        { k: qsTr("débit"), v: detail.st.mbps > 0 ? Number(detail.st.mbps).toFixed(1).replace(".", ",") + " Mb/s" : "—" },
                        { k: qsTr("hôte → écran"), v: panel.ms(detail.st.presentToDisplay), accent: true },
                        { k: qsTr("manette → ps5"), v: panel.ms(detail.st.inputToPs5), accent: true }]
                    delegate: ColumnLayout {
                        required property var modelData
                        spacing: 2
                        Label { text: modelData.k }
                        Text { text: modelData.v; color: modelData.accent ? Theme.accentText : Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                    }
                }
            }
            Meta { text: qsTr("il reçoit : %1").arg(panel.receives(detail.st)) + (detail.st.share ? " · " + qsTr("sa part : %1").arg(panel.mbps(detail.st.share)) : ""); color: Theme.accentText }
            Meta { text: detail.st.link > 0 ? qsTr("sa connexion : %1 · ping %2").arg(panel.mbps(detail.st.link)).arg(panel.ms(detail.st.linkRtt)) + (detail.st.hz ? " · " + qsTr("écran %1 Hz").arg(Math.round(detail.st.hz)) : "")
                                            : qsTr("sa connexion : test en cours ou pas encore fait") }
            Meta {
                text: qsTr("sa manette : %1").arg(!detail.st.pad ? qsTr("aucune") : detail.st.synthetic ? qsTr("test manette") : (detail.st.padName || qsTr("manette")))
                      + (detail.st.access ? " · " + qsTr("accès permanent") : "")
            }
            Meta {
                visible: detail.st.padSilent === true
                text: qsTr("sa manette envoie des données mais aucun bouton n'est arrivé : il peut la configurer depuis sa page.")
                color: Theme.warn
            }
        }

        // --- what they receive, at most --------------------------------------------------------
        ColumnLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("ce que reçoivent tes invités, au plus") }
            UiSeg {
                objectName: "share-feed"
                accessibleName: qsTr("ce que reçoit l'invité")
                cellHeight: 30
                options: [{ id: "real", label: qsTr("images réelles") }, { id: "60", label: qsTr("fluide 60") }, { id: "120", label: qsTr("fluide 120") }]
                current: panel.maxFeed
                onPicked: id => { if (!panel.preview) veyra.setPreference("guestFeed", id) }
            }
            UiSeg {
                objectName: "share-size"
                accessibleName: qsTr("définition")
                cellHeight: 26
                options: [{ id: "1080p", label: "1080p" }, { id: "native", label: qsTr("native") }]
                current: panel.maxSize === "720p" ? "" : panel.maxSize
                onPicked: id => { if (!panel.preview) veyra.setPreference("guestResolution", id) }
            }
            Meta {
                text: qsTr("celui qui a la manette reçoit des images réelles en automatique : les modes fluides attendent les images générées par ta carte (15 à 30 ms de plus).")
                color: Theme.t3
            }
            Label { text: qsTr("qui joue") }
            UiSeg {
                objectName: "share-mode"
                accessibleName: qsTr("qui joue")
                cellHeight: 30
                options: [{ id: "guest", label: qsTr("l'invité joue") }, { id: "together", label: qsTr("à deux") }, { id: "watch", label: qsTr("regarder seulement") }]
                current: panel.mode
                onPicked: id => { if (panel.g && !panel.preview) panel.g.setMode(id) }
            }
        }

        RowLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            UiButton { size: 32; text: qsTr("arrêter le partage"); onClicked: if (panel.g && !panel.preview) panel.g.stop() }
        }
    }
    }
}
