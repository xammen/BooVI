// Partager (fork): the stream toolbar's panel for one remote guest (GuestShareModel, `vyGuest`).
// Start sharing, copy the link, see who is connected and how, choose who drives the pad, kick.
// Through Cloudflare (the named tunnel or lien rapide) the tunnel opens with the share by itself;
// in the tailnet, `tailscale serve` only runs from the button here.
// `preview` shows sample values without a session (--page stream-preview --share-panel).
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: panel
    property bool open: false
    property bool preview: false
    signal closeRequested()
    readonly property bool videoCover: true
    property real coverRadius: Theme.rLg
    readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
    readonly property bool active: preview || (g !== null && g.active)
    readonly property string phase: preview ? "connected" : g ? g.phase : "off"
    readonly property var st: preview ? ({ rtt: 18.4, mbps: 47.6, fps: 120, path: qsTr("direct"), presentToDisplay: 31.2, inputToPs5: 14.8, width: 2560, height: 1440,
                                           pad: true, padName: "Steam Controller", padMapping: "steam-hid", feed: "120", size: "native", auto: true, kbps: 52000,
                                           link: 85000, linkRtt: 6.2, hz: 144, guestFps: 119.6, decodedToSentReal: 6.5, decodedToSentSmooth: 31.4 })
                                      : g ? g.stats : ({})
    // The host's maximum (Réglages → partage), changed here during a share too.
    readonly property string maxFeed: typeof veyra !== "undefined" && veyra.preferences.guestFeed ? veyra.preferences.guestFeed : "120"
    readonly property string maxSize: typeof veyra !== "undefined" && veyra.preferences.guestResolution ? veyra.preferences.guestResolution : "native"
    function feedText(f) { return f === "120" ? qsTr("fluide 120") : f === "60" ? qsTr("fluide 60") : qsTr("images réelles") }
    function mbps(kbps) { return kbps > 0 ? Number(kbps / 1000).toFixed(kbps < 10000 ? 1 : 0).replace(".", ",") + " Mb/s" : "—" }
    // What the guest receives now, as their choice says it.
    readonly property string receives: !st.feed ? ""
        : (st.auto ? qsTr("auto") : qsTr("manuel")) + " · " + feedText(st.feed) + " · " + (st.height ? st.height + "p" : "") + " · " + mbps(st.kbps)
          + (st.guestFps !== undefined && st.guestFps !== null ? " · " + Math.round(st.guestFps) + " i/s" : "")
    readonly property string linkText: st.link > 0 ? qsTr("sa connexion : %1 · ping %2").arg(mbps(st.link)).arg(ms(st.linkRtt)) + (st.hz ? " · " + qsTr("écran %1 Hz").arg(Math.round(st.hz)) : "")
                                                   : qsTr("sa connexion : test en cours ou pas encore fait")
    readonly property string reach: preview ? "tunnel" : g ? g.reach : "local"
    readonly property bool viaCloudflare: reach === "tunnel" || reach === "quick"
    readonly property string tunnel: preview ? "connected" : g ? g.tunnel : "off"
    readonly property string link: preview ? "https://play.example.com/?t=••••••••" : g ? g.link : ""
    readonly property string mode: preview ? "guest" : g ? g.mode : "guest"
    // The guest's pad as their page reports it, and what is wrong with it.
    readonly property string padText: !st.pad ? qsTr("aucune")
        : st.synthetic ? qsTr("test manette")
        : (st.padName ? st.padName : qsTr("manette")) + ({ "non-standard": " · " + qsTr("non standard"), "remap": " · " + qsTr("configurée"), "steam-hid": " · webhid" }[st.padMapping] || "")
    readonly property string padWarning: !st.pad ? qsTr("l'invité n'a pas de manette : sa page lui montre comment en brancher une.")
        : st.padSilent ? qsTr("sa manette envoie des données mais aucun bouton n'est arrivé : il peut la configurer depuis sa page.")
        : ""
    function ms(v) { return v === undefined || v === null || v < 0 ? "—" : Number(v).toFixed(1).replace(".", ",") + " ms" }

    width: 400
    height: col.implicitHeight + 40 + 1
    radius: Theme.rLg
    color: Theme.popover
    border.width: 1
    border.color: Theme.stroke2
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

    ColumnLayout {
        id: col
        x: 20
        y: 21
        width: parent.width - 40
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
        Text {
            visible: !panel.active
            Layout.fillWidth: true
            text: qsTr("un ami regarde ton stream et joue avec sa manette, depuis son navigateur. il lui faut le lien, chrome ou edge, et une manette branchée sur son pc.")
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsText
            lineHeight: 1.5
            wrapMode: Text.Wrap
        }
        UiButton {
            visible: !panel.active
            kind: "solid"
            size: 36
            text: qsTr("démarrer le partage")
            iconName: "users"
            onClicked: if (panel.g) panel.g.start()
        }
        Text {
            visible: !panel.active && panel.g !== null && panel.g.error.length > 0
            Layout.fillWidth: true
            text: panel.g ? panel.g.error : ""
            color: Theme.err
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            wrapMode: Text.Wrap
        }

        // --- sharing ---------------------------------------------------------------------
        RowLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 10
            UiDot { hollow: panel.phase !== "connected" }
            Text {
                Layout.fillWidth: true
                text: panel.phase === "connected" ? qsTr("invité connecté") + (panel.st.path ? " · " + panel.st.path : "")
                    : panel.phase === "connecting" ? qsTr("l'invité se connecte…")
                    : qsTr("en attente de l'invité")
                color: Theme.t1
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsText
                elide: Text.ElideRight
            }
        }
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
                    text: panel.link.length > 0 ? panel.link : qsTr("ouverture du tunnel…")
                    color: panel.link.length > 0 ? Theme.accentText : Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                    elide: Text.ElideMiddle
                }
            }
            UiButton { size: 32; text: qsTr("copier"); iconName: "copy"; enabled: panel.link.length > 0; onClicked: if (panel.g) panel.g.copyLink() }
        }
        Text {
            visible: panel.active
            Layout.fillWidth: true
            text: panel.viaCloudflare ? qsTr("ton ami ouvre ce lien dans chrome ou edge, il n'installe rien. quiconque a le lien peut rejoindre tant que le partage dure.")
                : panel.reach === "tailnet-https" ? qsTr("lien https de ton tailnet : ton ami doit y être, ou tu lui partages ce pc dans tailscale.")
                : panel.reach === "tailnet-http" ? qsTr("lien http de ton tailnet : l'image passe, mais les navigateurs ne lisent la manette qu'en https.")
                : qsTr("tailscale est coupé : seul ce pc peut ouvrir ce lien.")
            color: panel.reach === "tailnet-http" || panel.reach === "local" ? Theme.warn : Theme.t3
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            lineHeight: 1.4
            wrapMode: Text.Wrap
        }
        RowLayout {
            visible: panel.active && panel.viaCloudflare
            Layout.fillWidth: true
            spacing: 8
            UiDot { hollow: panel.tunnel !== "connected" && panel.tunnel !== "failed"; tone: panel.tunnel === "failed" ? Theme.err : Theme.ok }
            Text {
                Layout.fillWidth: true
                text: panel.tunnel === "connected" ? qsTr("tunnel cloudflare connecté")
                    : panel.tunnel === "failed" ? qsTr("le tunnel cloudflare s'est arrêté")
                    : qsTr("tunnel cloudflare en connexion…")
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
        }
        Flow {
            visible: panel.active && !panel.preview && !panel.viaCloudflare
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

        GridLayout {
            visible: panel.active && panel.phase === "connected"
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 16
            rowSpacing: 6
            Repeater {
                model: [
                    { k: qsTr("ping"), v: panel.ms(panel.st.rtt) },
                    { k: qsTr("débit"), v: panel.st.mbps > 0 ? Number(panel.st.mbps).toFixed(1).replace(".", ",") + " Mb/s" : "—" },
                    { k: qsTr("hôte → écran"), v: panel.ms(panel.st.presentToDisplay), accent: true },
                    { k: qsTr("manette → ps5"), v: panel.ms(panel.st.inputToPs5), accent: true }]
                delegate: ColumnLayout {
                    required property var modelData
                    spacing: 2
                    Text { text: modelData.k; color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsLabel }
                    Text { text: modelData.v; color: modelData.accent ? Theme.accentText : Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                }
            }
        }

        ColumnLayout {
            visible: panel.active && panel.phase === "connected"
            Layout.fillWidth: true
            spacing: 4
            Text {
                // A friend who came in with their own access (Réglages → invités).
                visible: (panel.st.access || "").length > 0
                Layout.fillWidth: true
                text: qsTr("invité : %1 · accès permanent").arg(panel.st.access || "")
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                // A friend who joined from their own BooVI: their GPU does the frame generation.
                visible: panel.st.client === "boovi"
                Layout.fillWidth: true
                text: qsTr("invité : application BooVI")
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("manette de l'invité : %1").arg(panel.padText)
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                visible: panel.padWarning.length > 0
                Layout.fillWidth: true
                text: panel.padWarning
                color: Theme.warn
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                lineHeight: 1.4
                wrapMode: Text.Wrap
            }
        }

        ColumnLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            Text { text: qsTr("ce que reçoit l'invité, au plus"); color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsLabel }
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
                options: [{ id: "1080p", label: "1080p" }, { id: "native", label: panel.st.sourceHeight > 1080 ? qsTr("native · %1p").arg(panel.st.sourceHeight) : qsTr("native") }]
                current: panel.maxSize === "720p" ? "" : panel.maxSize
                onPicked: id => { if (!panel.preview) veyra.setPreference("guestResolution", id) }
            }
            Text {
                visible: panel.phase === "connected" && panel.receives.length > 0
                Layout.fillWidth: true
                text: qsTr("il reçoit : %1").arg(panel.receives)
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                visible: panel.phase === "connected"
                Layout.fillWidth: true
                text: panel.linkText
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                // Measured on this PC: picture decoded here -> handed to the network, per tap.
                text: qsTr("décodée → envoyée : réelles %1 · fluide %2. les modes fluides attendent les images générées par ta carte (15 à 30 ms de plus).")
                      .arg(panel.ms(panel.st.decodedToSentReal)).arg(panel.ms(panel.st.decodedToSentSmooth))
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                lineHeight: 1.4
                wrapMode: Text.Wrap
            }
        }

        ColumnLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            Text { text: qsTr("la manette"); color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsLabel }
            UiSeg {
                objectName: "share-mode"
                accessibleName: qsTr("qui joue")
                cellHeight: 30
                options: [{ id: "guest", label: qsTr("l'invité joue") }, { id: "together", label: qsTr("à deux") }, { id: "watch", label: qsTr("regarder seulement") }]
                current: panel.mode
                onPicked: id => { if (panel.g && !panel.preview) panel.g.setMode(id) }
            }
            Text {
                visible: panel.mode === "watch" && panel.phase === "connected"
                Layout.fillWidth: true
                text: qsTr("regarder seulement : tout ce que l'invité appuie est ignoré.")
                color: Theme.warn
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            visible: panel.active
            Layout.fillWidth: true
            spacing: 8
            UiButton {
                visible: panel.phase === "connected"
                size: 32
                text: qsTr("couper l'invité")
                iconName: "userx"
                inkOverride: Theme.err
                onClicked: if (panel.g && !panel.preview) panel.g.kick()
            }
            Item { Layout.fillWidth: true }
            UiButton { size: 32; text: qsTr("arrêter le partage"); onClicked: if (panel.g && !panel.preview) panel.g.stop() }
        }
    }
}
