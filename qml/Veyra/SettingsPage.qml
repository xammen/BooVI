// Réglages (Settings mockup): a sidebar with the way back, a search and the sections; one
// section at a time on the right, in cards of rows. Every row is one bridge setting, with
// a sentence that says what it changes for the player and nothing about how.
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import QtQuick.Window

VPage {
    id: root
    signal requestPage(string page)
    dragStripHeight: 0
    property string section: "image"
    property string query: ""
    readonly property var ps5: veyra.ps5 || ({})
    StreamPresets { id: presets }
    Sources { id: sources }

    readonly property var sections: [
        { id: "image", label: qsTr("image"), keys: "image super résolution sr upscaling frame generation fg couleur luminosité contraste saturation dlss" },
        { id: "latency", label: qsTr("latence"), keys: "latence limite images fps synchronisation vsync tearing g-sync vrr reflex nvidia" },
        { id: "display", label: qsTr("affichage"), keys: "affichage écran hdr stats statistiques" },
        { id: "streaming", label: qsTr("streaming"), keys: "streaming ps5 résolution codec débit hevc" },
        { id: "audio", label: qsTr("audio"), keys: "audio son volume muet couper casque sortie haut-parleurs stéréo surround 5.1 synchro décalage retard" },
        { id: "pad", label: qsTr("manette"), keys: "manette dualsense gyroscope arrière-plan regarder" },
        { id: "kbm", label: qsTr("clavier-souris"), keys: "clavier souris vitesse caméra sensibilité précision visée fine courbe zone morte touches raccourcis kbm" },
        { id: "captures", label: qsTr("captures"), keys: "captures enregistrement vidéo replay capture d'écran screenshot micro" },
        { id: "partage", label: qsTr("partage"), keys: "partage partager invité ami distance jouer à deux tailscale cloudflare relais lien débit" },
        { id: "invites", label: qsTr("invités"), keys: "invités invité ami accès permanent lien révoquer accueil absence veille windows plateau notification" },
        { id: "app", label: qsTr("interface"), keys: "interface thème couleurs palette animations langue journal logs version" }
    ]
    readonly property var shown: query.length === 0 ? sections : sections.filter(s => (s.label + " " + s.keys).indexOf(query.toLowerCase()) >= 0)
    signal sectionEntered()
    onSectionChanged: { pick = section; content.contentY = 0; sectionEntered() }
    // The section the sidebar points at. The content fades out (90 ms) before it follows,
    // then rises in (UiEnter below); a click during the fade only changes where it lands.
    property string pick: section
    onPickChanged: if (pick !== section || sectionOut.running) sectionOut.restart()
    SequentialAnimation {
        id: sectionOut
        NumberAnimation { target: body; property: "opacity"; to: 0; duration: Theme.d(90); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
        ScriptAction {
            script: {
                const same = root.section === root.pick
                root.section = root.pick
                body.opacity = 1
                if (same) root.sectionEntered()
            }
        }
    }

    // Stages the old interface could switch on and this one no longer shows. While any of
    // them is still on, the image section names them and offers to turn them off.
    readonly property var legacyNames: ({ "nr": qsTr("débruitage"), "nr-stabilizer": qsTr("stabilisation"), "aa": qsTr("anti-crénelage"),
                                          "beauty": qsTr("beauté"), "protection": qsTr("zone protégée"), "exclusion": qsTr("zone exclue"),
                                          "video-hdr": "rtx video hdr" })
    readonly property var legacyOn: (veyra.chain || []).map((n, i) => ({ i: n.index !== undefined ? n.index : i, type: n.type, on: n.enabled === true }))
                                                    .filter(n => n.on && legacyNames[n.type] !== undefined)
    function colourParam(name) { return (veyra.colourParameters || []).find(p => p.name === name) || ({ min: 0, max: 2, step: 0.01, defaultValue: 1 }) }
    readonly property bool hasColour: presets.hasStage("color")

    // --- sidebar ---------------------------------------------------------------------------
    Rectangle {
        id: side
        width: 256
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        color: "transparent"
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
        DragHandler {
            target: null
            enabled: root.shell && !root.shell.fullscreen && !root.shell.maximized
            onActiveChanged: if (active) root.shell.startSystemMove()
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            anchors.topMargin: 24
            anchors.bottomMargin: 24
            spacing: 24
            // The bar above has accueil; during a stream this is the way back to the game.
            AbstractButton {
                id: backLink
                visible: sources.live.length > 0
                implicitHeight: 24
                implicitWidth: backRow.implicitWidth + 24
                hoverEnabled: true
                Accessible.name: backText.text
                onClicked: root.requestPage("min")
                background: Rectangle { radius: Theme.rMd; color: "transparent"; border.width: backLink.visualFocus ? 2 : 0; border.color: Theme.focusRing }
                contentItem: Row {
                    id: backRow
                    leftPadding: 12
                    spacing: 8
                    VIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "arrowleft"; size: 14; strokeWidth: 1.5
                        color: backText.color
                        transform: Translate { x: backLink.hovered ? -2 : 0; Behavior on x { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
                    }
                    Text {
                        id: backText
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("retour au jeu")
                        color: backLink.hovered ? Theme.t1 : Theme.t2
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsMeta
                        Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
            Text { Layout.leftMargin: 12; text: qsTr("réglages"); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: 20; font.letterSpacing: -0.4 }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 36
                radius: Theme.rMd
                color: Theme.card
                border.width: 1
                border.color: search.activeFocus ? Theme.accent : Theme.stroke
                Behavior on border.color { ColorAnimation { duration: Theme.d(150) } }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8
                    VIcon { name: "search"; size: 14; strokeWidth: 1.5; color: Theme.t3 }
                    Basic.TextField {
                        id: search
                        Layout.fillWidth: true
                        padding: 0
                        background: null
                        placeholderText: qsTr("chercher un réglage")
                        placeholderTextColor: Qt.rgba(Theme.t3.r, Theme.t3.g, Theme.t3.b, 0.75)
                        color: Theme.t1
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsText
                        Accessible.name: qsTr("chercher un réglage")
                        onTextChanged: root.query = text.trim()
                        onAccepted: if (root.shown.length > 0) root.pick = root.shown[0].id
                    }
                }
            }
            Item {
                id: nav
                Layout.fillWidth: true
                implicitHeight: navCol.implicitHeight
                readonly property int at: root.shown.findIndex(s => s.id === root.pick)
                Rectangle {
                    width: parent.width
                    height: 40
                    radius: Theme.rMd
                    color: Theme.selBg
                    border.width: 1
                    border.color: Theme.selLine
                    visible: nav.at >= 0
                    y: Math.max(0, nav.at) * 40
                    Behavior on y { NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                }
                Column {
                    id: navCol
                    width: parent.width
                    Repeater {
                        model: root.shown
                        delegate: AbstractButton {
                            id: navItem
                            required property var modelData
                            readonly property bool here: root.pick === modelData.id
                            width: navCol.width
                            height: 40
                            hoverEnabled: true
                            Accessible.name: modelData.label
                            onClicked: root.pick = modelData.id
                            scale: pressed ? Theme.pressScale : 1
                            Behavior on scale { NumberAnimation { duration: Theme.d(navItem.pressed ? 100 : 200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                            background: Item { UiFocusRing { innerRadius: Theme.rMd; gap: -2; visible: navItem.visualFocus } }
                            contentItem: Text {
                                leftPadding: 14
                                verticalAlignment: Text.AlignVCenter
                                text: navItem.modelData.label
                                color: navItem.here || navItem.hovered ? Theme.t1 : Theme.t2
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsRow
                                Behavior on color { ColorAnimation { duration: Theme.d(Theme.durHover) } }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                        }
                    }
                }
                Text {
                    visible: root.shown.length === 0
                    leftPadding: 12
                    text: qsTr("rien ne correspond")
                    color: Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                }
            }
            Item { Layout.fillHeight: true }
            Text {
                Layout.leftMargin: 12
                text: "BooVI " + veyra.version + " · gpl-3.0"
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsLabel
            }
        }
    }

    // --- content -----------------------------------------------------------------------------
    Item {
        id: main
        anchors.left: side.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        UiDither {
            anchors.left: parent.left
            anchors.right: parent.right
            height: 112
            cell: 2
            mode: "band"
            opacity: 0.45
        }
        DragHandler {
            target: null
            enabled: root.shell && !root.shell.fullscreen && !root.shell.maximized
            onActiveChanged: if (active) root.shell.startSystemMove()
        }
        Flickable {
            id: content
            anchors.fill: parent
            contentWidth: width
            contentHeight: body.implicitHeight + 96
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: VScrollBar {}
            ColumnLayout {
                id: body
                width: Math.min(720, content.width - 64)
                x: Math.round((content.width - width) / 2)
                y: 48
                spacing: 24

                ColumnLayout {
                    id: heading
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: (root.sections.find(s => s.id === root.section) || {}).label || ""
                        color: Theme.t1
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTitle
                        font.letterSpacing: -1
                    }
                    Text {
                        Layout.fillWidth: true
                        text: ({ image: qsTr("ce que ta carte graphique fait du flux."),
                                 latency: qsTr("le chemin le plus court entre le flux et ton écran."),
                                 display: qsTr("hdr et infos à l'écran pendant le jeu."),
                                 streaming: qsTr("ce que BooVI demande à la ps5."),
                                 audio: qsTr("où sort le son du stream, et à quel niveau."),
                                 pad: qsTr("la manette que la console reçoit."),
                                 kbm: qsTr("jouer à la souris, convertie en manette."),
                                 captures: qsTr("enregistrer, garder les dernières secondes, prendre une image."),
                                 partage: qsTr("tes amis regardent ton stream, un seul tient la manette, depuis leur navigateur ou BooVI."),
                                 invites: qsTr("un ami allume ta ps5 et joue avec son propre lien, même quand tu n'es pas là."),
                                 app: qsTr("l'apparence de BooVI.") })[root.section]
                        color: Theme.t2
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsRow
                        wrapMode: Text.Wrap
                    }
                }

                // image -----------------------------------------------------------------
                ColumnLayout {
                    id: imageSection
                    visible: root.section === "image"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("super résolution")
                            desc: veyra.srEnabled ? qsTr("agrandit le flux vers %1.").arg(presets.targetLabel(veyra.srTargetIndex)) : qsTr("le flux s'affiche à sa taille d'origine.")
                            UiSwitch {
                                objectName: "set-sr"
                                on: veyra.srEnabled
                                onSwitched: v => { if (v) presets.ensureStage("sr"); veyra.srEnabled = v }
                            }
                        }
                        UiRow {
                            visible: veyra.srEnabled
                            title: qsTr("taille visée")
                            desc: qsTr("la taille de ton écran est le bon choix.")
                            UiSeg {
                                options: [{ id: "1", label: "1440p" }, { id: "2", label: "4k" }, { id: "4", label: "5k" }, { id: "3", label: "8k" }]
                                current: String(veyra.srTargetIndex)
                                onPicked: id => veyra.srTargetIndex = Number(id)
                            }
                        }
                        UiRow {
                            visible: veyra.srEnabled
                            title: qsTr("méthode")
                            UiSelect {
                                implicitWidth: 220
                                options: veyra.srBackendChoices || []
                                current: veyra.srAlgorithmOf(veyra.videoSrQuality)
                                onPicked: id => veyra.videoSrQuality = veyra.srQualityFor(id, veyra.srTierOf(veyra.videoSrQuality))
                            }
                        }
                        UiRow {
                            title: qsTr("frame generation")
                            desc: qsTr("plus fluide, mais chaque vraie image attend un peu. à couper dans les jeux compétitifs.")
                            UiSeg {
                                objectName: "set-fg"
                                options: [{ id: "1", label: qsTr("off") }].concat(presets.multipliers)
                                current: veyra.fgEnabled ? String(veyra.fgMultiplier) : "1"
                                onPicked: id => {
                                    if (id === "1") { veyra.fgEnabled = false; return }
                                    presets.ensureStage("frame-generation")
                                    veyra.setPreference("fluidMultiplier", Number(id))
                                    veyra.fgMultiplier = Number(id)
                                }
                            }
                        }
                        UiRow {
                            visible: veyra.fgEnabled
                            title: qsTr("jeu à 30 i/s sur la ps5")
                            desc: qsTr("la ps5 envoie chaque image en double. les doublons sont retirés : fluide garde la sortie choisie (x2 devient x4), réactif garde le multiplicateur et raccourcit l'attente d'environ 8 ms.")
                            UiSeg {
                                objectName: "set-half-rate"
                                options: [{ id: "double", label: qsTr("fluide") }, { id: "keep", label: qsTr("réactif") }]
                                current: veyra.halfRateKeepsMultiplier ? "keep" : "double"
                                onPicked: id => veyra.halfRateKeepsMultiplier = id === "keep"
                            }
                        }
                        UiRow {
                            title: qsTr("anticipation caméra · partage rejoint")
                            desc: qsTr("chez un ami : la caméra suit ton stick droit à chaque rafraîchissement de ton écran, sans attendre l'image suivante. seule : pas de frame gen, donc aucune attente, mais les objets bougent au rythme du jeu. avec interpolation : la frame gen lisse aussi les objets et remet son attente (une demi-image à une image). automatique : seule, sur un écran de 100 hz ou plus.")
                            UiSelect {
                                objectName: "set-camera-anticipation"
                                implicitWidth: 220
                                options: [{ id: "0", label: qsTr("automatique") }, { id: "1", label: qsTr("désactivée") },
                                          { id: "2", label: qsTr("anticipation seule") }, { id: "3", label: qsTr("anticipation + interpolation") }]
                                current: String(veyra.cameraAnticipation)
                                onPicked: id => veyra.cameraAnticipation = Number(id)
                            }
                        }
                        UiRow {
                            visible: veyra.fgEnabled
                            title: qsTr("méthode de frame gen")
                            // Greyed choices say why here: the list itself has no room for the reason.
                            readonly property string unavailable: (veyra.fgBackendChoices || []).filter(o => o.disabled && o.note)
                                .map(o => o.label.split(" · ")[0] + " : " + o.note).join("\n")
                            desc: veyra.fgDisplayHint.length > 0 ? veyra.fgDisplayHint : unavailable
                            descColor: veyra.fgDisplayHint.length > 0 ? Theme.warn : Theme.t2
                            UiSelect {
                                implicitWidth: 220
                                options: veyra.fgBackendChoices || []
                                current: veyra.fgBackendName
                                onPicked: id => veyra.fgBackendName = id
                            }
                        }
                    }
                    ColumnLayout {
                        visible: veyra.fgEnabled
                        Layout.fillWidth: true
                        spacing: 8
                        UiFieldLabel { text: qsTr("anti-artefacts") }
                        UiCard {
                            UiRow {
                                first: true
                                title: qsTr("protéger le viseur et le hud")
                                desc: qsTr("le viseur, le texte et la minimap restent nets au lieu d'être dédoublés par la frame gen.")
                                UiSwitch {
                                    objectName: "set-fg-hud-guard"
                                    on: veyra.fgHudGuard
                                    onSwitched: v => veyra.fgHudGuard = v
                                }
                            }
                            UiRow {
                                title: qsTr("mouvements rapides : privilégier la netteté")
                                desc: qsTr("quand la caméra tourne, l'image suit son mouvement sans lignes. sur un geste trop brusque, la vraie image reste affichée plutôt qu'une image ratée.")
                                UiSwitch {
                                    objectName: "set-fg-fast-guard"
                                    on: veyra.fgFastGuard
                                    onSwitched: v => veyra.fgFastGuard = v
                                }
                            }
                        }
                    }
                    UiCard {
                        visible: root.legacyOn.length > 0
                        UiRow {
                            first: true
                            title: qsTr("autres effets actifs")
                            desc: qsTr("%1, réglés avant cette version. ils ajoutent du travail à chaque image.").arg(root.legacyOn.map(n => root.legacyNames[n.type]).filter((v, k, a) => a.indexOf(v) === k).join(", "))
                            UiButton { text: qsTr("tout couper"); size: 32; onClicked: { for (const n of root.legacyOn.slice().reverse()) veyra.setEffectEnabled(n.i, false) } }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("couleur")
                            desc: root.hasColour ? qsTr("luminosité, contraste et saturation de l'image.") : qsTr("ajuste la luminosité, le contraste et la saturation.")
                            UiSwitch {
                                on: root.hasColour && veyra.colorEnabled
                                onSwitched: v => { if (v) presets.ensureStage("color"); veyra.colorEnabled = v }
                            }
                        }
                        GridLayout {
                            visible: root.hasColour && veyra.colorEnabled
                            Layout.fillWidth: true
                            Layout.leftMargin: 24
                            Layout.rightMargin: 24
                            Layout.topMargin: 8
                            Layout.bottomMargin: 24
                            columns: 3
                            columnSpacing: 32
                            UiSlider {
                                Layout.preferredWidth: 1
                                label: qsTr("luminosité")
                                from: -1; to: 1; stepSize: 0.05
                                value: veyra.colorExposure
                                format: v => (v > 0 ? "+" : "") + v.toFixed(2).replace(".", ",") + " ev"
                                onCommitted: v => veyra.colorExposure = v
                            }
                            UiSlider {
                                Layout.preferredWidth: 1
                                label: qsTr("contraste")
                                readonly property var p: root.colourParam("contrast")
                                from: p.min; to: p.max; stepSize: p.step
                                value: veyra.colorContrast
                                format: v => v.toFixed(2).replace(".", ",")
                                onCommitted: v => veyra.colorContrast = v
                            }
                            UiSlider {
                                Layout.preferredWidth: 1
                                label: qsTr("saturation")
                                readonly property var p: root.colourParam("saturation")
                                from: p.min; to: p.max; stepSize: p.step
                                value: veyra.colorSaturation
                                format: v => v.toFixed(2).replace(".", ",")
                                onCommitted: v => veyra.colorSaturation = v
                            }
                        }
                    }
                }

                // latency ---------------------------------------------------------------
                ColumnLayout {
                    id: latencySection
                    visible: root.section === "latency"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("limite d'images")
                            desc: veyra.outputRateMode === 1 ? qsTr("jamais plus d'images que ton écran ne peut en montrer.") : veyra.outputRateMode === 0 ? qsTr("aucune limite : des images peuvent être calculées pour rien.") : qsTr("une limite fixe, en images par seconde.")
                            enabled: !veyra.presentationOwned
                            UiSeg {
                                objectName: "set-output-rate"
                                options: [{ id: "0", label: qsTr("aucune") }, { id: "1", label: qsTr("l'écran") }, { id: "2", label: qsTr("fixe") }]
                                current: String(veyra.outputRateMode)
                                onPicked: id => veyra.outputRateMode = Number(id)
                            }
                        }
                        UiRow {
                            visible: veyra.outputRateMode === 2
                            title: qsTr("images par seconde")
                            UiField {
                                implicitWidth: 96
                                fieldHeight: 32
                                text: Math.round(veyra.outputCustomFps * 1000) / 1000
                                onEdited: t => { const v = Number(t.replace(",", ".")); if (isFinite(v)) veyra.outputCustomFps = v }
                            }
                        }
                        UiRow {
                            title: qsTr("synchronisation")
                            desc: veyra.displaySync === 2 ? qsTr("auto : pas de déchirure et pas d'attente.") : veyra.displaySync === 1 ? qsTr("chaque image attend le rafraîchissement de l'écran.") : qsTr("le plus rapide, avec des déchirures possibles sans g-sync.")
                            UiSeg {
                                options: [{ id: "2", label: qsTr("auto") }, { id: "1", label: "v-sync" }, { id: "0", label: qsTr("déchirure") }]
                                current: String(veyra.displaySync)
                                onPicked: id => veyra.displaySync = Number(id)
                            }
                        }
                        UiRow {
                            title: "g-sync"
                            desc: veyra.vrrState >= 3 ? qsTr("l'écran suit le rythme des images.") : veyra.vrrState === 1 ? qsTr("cet écran ne le gère pas.") : veyra.vrrState === 2 ? qsTr("l'écran le gère, mais pas dans ce mode d'affichage.") : qsTr("détection seulement sur carte nvidia.")
                            UiSwitch { enabled: veyra.vrrState >= 2; on: veyra.variableRefresh; onSwitched: v => veyra.variableRefresh = v }
                        }
                        UiRow {
                            visible: veyra.nvidiaLowLatencyAvailable
                            title: qsTr("nvidia reflex")
                            desc: qsTr("réduit la file d'attente de la carte graphique.")
                            UiSwitch { on: veyra.nvidiaLowLatency > 0; onSwitched: v => veyra.nvidiaLowLatency = v ? 1 : 0 }
                        }
                        UiRow {
                            visible: !veyra.nvidiaLowLatencyAvailable
                            title: qsTr("file d'attente courte")
                            desc: qsTr("moins d'images en attente avant l'écran.")
                            UiSwitch { on: veyra.fgLowQueue; onSwitched: v => veyra.fgLowQueue = v }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: veyra.presentationStatus.length > 0 && veyra.presentationOwned
                        text: veyra.presentationStatus
                        color: Theme.t3
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsText
                        wrapMode: Text.Wrap
                    }
                }

                // display ---------------------------------------------------------------
                ColumnLayout {
                    id: displaySection
                    visible: root.section === "display"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("écran")
                            desc: qsTr("l'écran où s'ouvre le stream.")
                            Text {
                                text: [String(Screen.model || Screen.name).trim().toLowerCase(),
                                       Math.round(Screen.width * Screen.devicePixelRatio) + "×" + Math.round(Screen.height * Screen.devicePixelRatio),
                                       veyra.displayRefreshHz > 1 ? Math.round(veyra.displayRefreshHz) + " hz" : ""].filter(x => x.length).join(" · ")
                                color: Theme.accentText
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsCtl
                            }
                        }
                        UiRow {
                            title: "hdr"
                            desc: veyra.displayHdr > 0 ? qsTr("windows affiche en hdr sur cet écran.") : veyra.displayHdr === 0 ? qsTr("active le hdr dans windows pour l'utiliser ici.") : qsTr("état du hdr inconnu pour l'instant.")
                            Text {
                                text: veyra.displayHdr > 0 ? qsTr("actif") : veyra.displayHdr === 0 ? qsTr("inactif") : "—"
                                color: veyra.displayHdr > 0 ? Theme.ok : Theme.t3
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsCtl
                            }
                        }
                        UiRow {
                            visible: veyra.displayHdr > 0
                            title: qsTr("sortie hdr")
                            desc: qsTr("hdr10 convient à presque tous les écrans.")
                            UiSeg { options: [{ id: "0", label: "hdr10" }, { id: "1", label: "scrgb" }]; current: String(veyra.hdrOutputMode); onPicked: id => veyra.hdrOutputMode = Number(id) }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("stats en jeu")
                            desc: qsTr("images reçues et affichées, en haut à droite. réduit, il ne montre que les i/s.")
                            UiSeg {
                                objectName: "set-stats"
                                options: [{ id: "full", label: qsTr("complet") }, { id: "compact", label: qsTr("réduit") }, { id: "off", label: qsTr("masqué") }]
                                current: {
                                    const m = veyra.preferences.streamStatsMode
                                    return m === "full" || m === "compact" || m === "off" ? m : (veyra.preferences.streamStats === false ? "off" : "full")
                                }
                                onPicked: id => veyra.setPreference("streamStatsMode", id)
                            }
                        }
                        UiRow {
                            title: qsTr("opacité des stats")
                            desc: qsTr("plus discret par-dessus le jeu. au survol, elles repassent à 100 %.")
                            UiSeg {
                                options: [{ id: "100", label: "100 %" }, { id: "70", label: "70 %" }, { id: "45", label: "45 %" }]
                                current: {
                                    const v = Number(veyra.preferences.streamStatsOpacity)
                                    return v > 85 || !(v > 0) ? "100" : v > 55 ? "70" : "45"
                                }
                                onPicked: id => veyra.setPreference("streamStatsOpacity", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("stats détaillées")
                            desc: qsTr("ajoute le temps de chaque étape sur la carte graphique.")
                            UiSwitch { on: veyra.preferences.streamStatsDetailed === true; onSwitched: v => veyra.setPreference("streamStatsDetailed", v) }
                        }
                    }
                }

                // streaming -------------------------------------------------------------
                ColumnLayout {
                    id: streamingSection
                    visible: root.section === "streaming"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("résolution et fréquence")
                            desc: qsTr("le maximum que la ps5 envoie en remote play.")
                            UiSeg {
                                options: [{ id: "1", label: "720p60" }, { id: "2", label: "1080p30" }, { id: "3", label: "1080p60" }]
                                current: String(root.ps5.quality === undefined ? 3 : root.ps5.quality)
                                onPicked: id => veyra.ps5Set("quality", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("codec")
                            desc: qsTr("h.265 donne une meilleure image au même débit.")
                            UiSeg {
                                options: [{ id: "0", label: "h.264" }, { id: "1", label: "h.265" }, { id: "2", label: "h.265 hdr" }]
                                current: String(root.ps5.codec || 0)
                                onPicked: id => veyra.ps5Set("codec", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("débit demandé")
                            desc: qsTr("la ps5 décide du débit réel ; c'est une demande, pas une garantie.")
                            UiSeg {
                                options: [{ id: "4", label: "30" }, { id: "5", label: "50" }, { id: "7", label: qsTr("100 Mb/s") }]
                                current: String(root.ps5.bitrate || 0)
                                onPicked: id => veyra.ps5Set("bitrate", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("décodage")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("auto") }, { id: "2", label: qsTr("carte graphique") }, { id: "1", label: qsTr("processeur") }]
                                current: String(root.ps5.decode || 0)
                                onPicked: id => veyra.ps5Set("decode", Number(id))
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("le pc et la xbox gardent leurs réglages de flux dans consoles.")
                        color: Theme.t3
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsText
                        wrapMode: Text.Wrap
                    }
                }

                // audio -----------------------------------------------------------------
                ColumnLayout {
                    id: audioSection
                    visible: root.section === "audio"
                    Layout.fillWidth: true
                    spacing: 24
                    onVisibleChanged: if (visible) veyra.refreshAudioDevices()
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("sortie")
                            desc: veyra.audioOutputFallback ? qsTr("la sortie choisie est débranchée : le son passe par celle de windows.") : qsTr("casque, haut-parleurs ou écran.")
                            descColor: veyra.audioOutputFallback ? Theme.warn : Theme.t2
                            UiSelect {
                                objectName: "set-audio-device"
                                implicitWidth: 260
                                options: veyra.audioDevices.map(d => ({
                                    id: d.id,
                                    label: d.id === "" ? qsTr("celle de windows")
                                         : d.missing ? qsTr("appareil enregistré, débranché")
                                         : String(d.name).toLowerCase() + (d.isDefault ? qsTr(" · windows") : "")
                                }))
                                current: veyra.preferences.audioDevice || ""
                                onPicked: id => veyra.setPreference("audioDevice", id)
                            }
                        }
                        UiRow {
                            title: qsTr("stéréo forcée")
                            desc: qsTr("le 5.1 du pc ou de la xbox, mixé pour un casque. la ps5 envoie déjà de la stéréo.")
                            UiSwitch { objectName: "set-audio-stereo"; on: veyra.preferences.audioForceStereo === true; onSwitched: v => veyra.setPreference("audioForceStereo", v) }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("couper le son")
                            desc: qsTr("jusqu'à la fermeture de BooVI. aussi dans la barre du stream.")
                            UiSwitch { objectName: "set-audio-mute"; on: veyra.muted; onSwitched: v => veyra.muted = v }
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
                        UiSlider {
                            objectName: "set-audio-volume"
                            Layout.margins: 24
                            Layout.topMargin: 16
                            label: qsTr("volume du stream")
                            from: 0; to: 100
                            suffix: " %"
                            value: Math.round(veyra.volume * 100)
                            onCommitted: v => veyra.volume = v / 100
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("synchro son et image")
                            desc: veyra.audioSyncMode === 0 ? (veyra.audioSyncLive ? qsTr("le son attend l'image : %1 ms en ce moment.").arg(Math.round(veyra.audioCompensationMs)) : qsTr("le son attend l'image le temps qu'elle soit traitée."))
                                : veyra.audioSyncMode === 1 ? qsTr("un retard fixe, à régler à l'oreille.")
                                : qsTr("le son part dès qu'il arrive, même en avance sur l'image.")
                            UiSeg {
                                objectName: "set-audio-sync"
                                options: [{ id: "0", label: qsTr("auto") }, { id: "1", label: qsTr("manuel") }, { id: "2", label: qsTr("aucune") }]
                                current: String(veyra.audioSyncMode)
                                onPicked: id => veyra.audioSyncMode = Number(id)
                            }
                        }
                        Rectangle { visible: veyra.audioSyncMode === 1; Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
                        UiSlider {
                            visible: veyra.audioSyncMode === 1
                            objectName: "set-audio-offset"
                            Layout.margins: 24
                            Layout.topMargin: 16
                            label: qsTr("retard du son")
                            from: 0; to: 250; stepSize: 10
                            suffix: " ms"
                            value: Math.max(0, veyra.audioOffsetMs)
                            onCommitted: v => veyra.audioOffsetMs = Math.round(v)
                        }
                    }
                }

                // pad -------------------------------------------------------------------
                ColumnLayout {
                    id: padSection
                    visible: root.section === "pad"
                    Layout.fillWidth: true
                    spacing: 24
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 72
                        radius: Theme.rLg
                        color: Theme.card
                        border.width: 1
                        border.color: root.ps5.controller === true ? Theme.selLine : Theme.stroke
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 24
                            anchors.rightMargin: 24
                            spacing: 16
                            VIcon { name: "pad"; size: 20; strokeWidth: 1.5; color: root.ps5.controller === true ? Theme.accentText : Theme.t3 }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Text { Layout.fillWidth: true; text: root.ps5.controller === true ? String(root.ps5.pad || qsTr("manette")).toLowerCase() : qsTr("aucune manette détectée"); color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsRow; elide: Text.ElideRight }
                                Text { Layout.fillWidth: true; text: root.ps5.controller === true ? (root.ps5.gyro === true ? qsTr("gyro · touchpad · vibrations") : qsTr("sans gyroscope")) : qsTr("branche-la en usb ou bluetooth."); color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta; elide: Text.ElideRight }
                            }
                            Text { visible: root.ps5.controller === true; text: qsTr("utilisée"); color: Theme.ok; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("manette en arrière-plan")
                            desc: qsTr("continue d'envoyer la manette quand tu cliques sur ton autre écran.")
                            UiSwitch { objectName: "set-ps5-background"; on: veyra.preferences.ps5ControlBackground === true; onSwitched: v => veyra.ps5ControlSet("background", v) }
                        }
                        UiRow {
                            title: qsTr("regarder seulement")
                            desc: qsTr("la ps5 ne reçoit aucun bouton. un badge le rappelle pendant le stream.")
                            UiSwitch { on: root.ps5.viewOnly === true; onSwitched: v => veyra.ps5Set("viewOnly", v) }
                        }
                        UiRow {
                            title: qsTr("gyroscope")
                            desc: root.ps5.controller === true ? qsTr("pose la manette à plat pendant 2 secondes.") : qsTr("branche une manette pour le calibrer.")
                            UiButton { text: root.ps5.calibrating === true ? qsTr("calibrage…") : qsTr("calibrer"); size: 32; enabled: root.ps5.controller === true; onClicked: veyra.ps5Calibrate() }
                        }
                    }
                }

                // kbm -------------------------------------------------------------------
                ColumnLayout {
                    id: kbmSection
                    visible: root.section === "kbm"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("la souris pilote")
                            desc: qsTr("le gyroscope ne sert que dans les jeux qui proposent la visée au gyro.")
                            UiSeg {
                                objectName: "set-kbm-mouse"
                                options: [{ id: "stick", label: qsTr("stick droit") }, { id: "gyro", label: qsTr("gyroscope") }]
                                current: veyra.preferences.kbmMouse === "gyro" ? "gyro" : "stick"
                                onPicked: id => veyra.setPreference("kbmMouse", id)
                            }
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
                        UiSlider {
                            objectName: "set-kbm-speed"
                            Layout.margins: 24
                            Layout.topMargin: 16
                            label: qsTr("vitesse de la caméra")
                            from: 0; to: 100
                            value: veyra.preferences.kbmSpeed !== undefined ? veyra.preferences.kbmSpeed : 50
                            // The mouse speed that gives full tilt (KbmSettings::fullTilt).
                            format: v => qsTr("%1 · plein régime à %2 pts/s").arg(Math.round(v)).arg(Math.round(3000 * Math.pow(200 / 3000, v / 100)))
                            onCommitted: v => veyra.setPreference("kbmSpeed", Math.round(v))
                        }
                        UiSlider {
                            objectName: "set-kbm-precision"
                            Layout.margins: 24
                            Layout.topMargin: 0
                            label: qsTr("visée fine")
                            from: 0; to: 100
                            value: veyra.preferences.kbmPrecision !== undefined ? veyra.preferences.kbmPrecision : 50
                            // KbmSettings::precisionKnee: below it each count turns the camera by the same amount.
                            format: v => Math.round(v) === 0 ? qsTr("0 · coupée")
                                       : qsTr("%1 · douce sous %2 pts/s").arg(Math.round(v)).arg(Math.round(0.25 * v / 100 * 3000 * Math.pow(200 / 3000, (veyra.preferences.kbmSpeed !== undefined ? veyra.preferences.kbmSpeed : 50) / 100)))
                            onCommitted: v => veyra.setPreference("kbmPrecision", Math.round(v))
                        }
                        UiRow {
                            title: qsTr("avancé")
                            desc: qsTr("courbe et zone morte du stick, à régler jeu par jeu.")
                            UiButton {
                                objectName: "set-kbm-advanced"
                                kind: "tool"
                                size: 32
                                text: kbmAdvanced.visible ? qsTr("masquer") : qsTr("afficher")
                                onClicked: kbmAdvanced.visible = !kbmAdvanced.visible
                            }
                        }
                        GridLayout {
                            id: kbmAdvanced
                            visible: false
                            Layout.fillWidth: true
                            Layout.margins: 24
                            Layout.topMargin: 0
                            columns: 2
                            columnSpacing: 32
                            UiSlider {
                                objectName: "set-kbm-curve"
                                Layout.preferredWidth: 1
                                label: qsTr("courbe")
                                from: 0; to: 100
                                value: veyra.preferences.kbmCurve !== undefined ? veyra.preferences.kbmCurve : 80
                                onCommitted: v => veyra.setPreference("kbmCurve", Math.round(v))
                            }
                            UiSlider {
                                objectName: "set-kbm-deadzone"
                                Layout.preferredWidth: 1
                                label: qsTr("zone morte du jeu")
                                from: 0; to: 40
                                suffix: " %"
                                value: veyra.preferences.kbmDeadzone !== undefined ? veyra.preferences.kbmDeadzone : 24
                                onCommitted: v => veyra.setPreference("kbmDeadzone", Math.round(v))
                            }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("touches")
                            desc: qsTr("ctrl+m les affiche aussi pendant le stream.")
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
                        KbmBindings {
                            Layout.fillWidth: true
                            Layout.margins: 24
                            bindings: kbmSection.visible ? (veyra.preferences.kbmMouse, veyra.ps5KbmBindings()) : []
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("en remote play, la ps5 ne reçoit qu'une manette : la souris devient un stick, pas une vraie souris. au-delà du plein régime, c'est la vitesse max du jeu qui limite : monte aussi la sensibilité dans le jeu. ctrl+m montre les touches, ctrl+alt+maj+z libère la souris.")
                        color: Theme.t3
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsText
                        lineHeight: 1.6
                        wrapMode: Text.Wrap
                    }
                }

                // captures --------------------------------------------------------------
                ColumnLayout {
                    id: capturesSection
                    visible: root.section === "captures"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("enregistrements")
                            desc: veyra.recordingDirectory
                            descColor: Theme.t3
                            UiButton { text: qsTr("ouvrir"); size: 32; onClicked: veyra.openRecordingDirectory() }
                            UiButton { text: qsTr("changer"); size: 32; onClicked: veyra.chooseRecordingDirectory() }
                        }
                        UiRow {
                            title: qsTr("qualité")
                            desc: qsTr("auto suit la taille et la fluidité de l'image. en Mb/s.")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("auto") }, { id: "20", label: "20" }, { id: "40", label: "40" }, { id: "80", label: "80" }]
                                current: String(veyra.recordingBitrateMbps)
                                onPicked: id => veyra.setPreference("recordBitrateMbps", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("images par seconde")
                            desc: qsTr("suivre garde les images de la frame gen.")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("suivre") }, { id: "60", label: "60" }, { id: "120", label: "120" }]
                                current: String(veyra.recordingFpsLimit)
                                onPicked: id => veyra.setPreference("recordFps", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("codec")
                            desc: qsTr("le hdr s'enregistre toujours en hevc 10 bits.")
                            UiSeg { options: [{ id: "0", label: "h.264" }, { id: "1", label: "hevc" }]; current: veyra.recordingHevc ? "1" : "0"; onPicked: id => veyra.setPreference("recordHevc", id === "1") }
                        }
                        UiRow {
                            title: qsTr("son du jeu")
                            UiSwitch { on: veyra.recordingAudio; onSwitched: v => veyra.setPreference("recordAudio", v) }
                        }
                        UiRow {
                            title: qsTr("micro")
                            desc: qsTr("mélangé au son du jeu.")
                            UiSwitch { on: veyra.recordingMicrophone; onSwitched: v => { veyra.setPreference("recordMic", v); if (v) veyra.refreshMicrophones() } }
                        }
                    }
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("replay")
                            desc: qsTr("garde les dernières secondes en mémoire. %1 les enregistre.").arg((veyra.shortcuts.replay || "").toLowerCase())
                            UiSwitch { on: veyra.replayEnabled; onSwitched: v => veyra.setPreference("replayEnabled", v) }
                        }
                        UiRow {
                            visible: veyra.replayEnabled
                            title: qsTr("durée")
                            UiSeg {
                                options: [{ id: "30", label: "30 s" }, { id: "60", label: "1 min" }, { id: "120", label: "2 min" }, { id: "300", label: "5 min" }]
                                current: String(veyra.replaySeconds)
                                onPicked: id => veyra.setPreference("replaySeconds", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("captures d'écran")
                            desc: veyra.screenshotDirectory
                            descColor: Theme.t3
                            UiButton { text: qsTr("ouvrir"); size: 32; onClicked: veyra.openScreenshotDirectory() }
                            UiButton { text: qsTr("changer"); size: 32; onClicked: veyra.chooseScreenshotDirectory() }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("raccourcis : %1 capture d'écran · %2 enregistrer · %3 replay")
                              .arg((veyra.shortcuts.screenshot || "—").toLowerCase()).arg((veyra.shortcuts.record || "—").toLowerCase()).arg((veyra.shortcuts.replay || "—").toLowerCase())
                        color: Theme.t3
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsMeta
                        wrapMode: Text.Wrap
                    }
                }

                // partage ---------------------------------------------------------------
                ColumnLayout {
                    id: shareSection
                    visible: root.section === "partage"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("ce que reçoit l'invité")
                            desc: qsTr("le plus que ton ami peut choisir. images réelles : la latence la plus basse. fluide : les images que ta carte génère, avec 15 à 30 ms d'attente en plus. sa page choisit dans cette limite, seule selon sa connexion ou à la main.")
                            UiSeg {
                                options: [{ id: "real", label: qsTr("images réelles") }, { id: "60", label: qsTr("fluide 60") }, { id: "120", label: qsTr("fluide 120") }]
                                current: veyra.preferences.guestFeed || "120"
                                onPicked: id => veyra.setPreference("guestFeed", id)
                            }
                        }
                        UiRow {
                            title: qsTr("définition")
                            desc: qsTr("native : la sortie de ta super résolution (1440p par exemple), sans copie de plus. 720p demande deux fois moins de débit que 1080p.")
                            UiSeg {
                                options: [{ id: "720p", label: "720p" }, { id: "1080p", label: "1080p" }, { id: "native", label: qsTr("native") }]
                                current: veyra.preferences.guestResolution || "native"
                                onPicked: id => veyra.setPreference("guestResolution", id)
                            }
                        }
                        UiRow {
                            title: qsTr("envoi max")
                            desc: qsTr("ce que ta connexion envoie au plus, en Mb/s, partagé entre tes invités : celui qui a la manette passe d'abord. leurs pages ne dépassent jamais ça ; BooVI baisse aussi tout seul quand des paquets se perdent.")
                            UiSeg {
                                options: [{ id: "20", label: "20" }, { id: "35", label: "35" }, { id: "50", label: "50" }, { id: "80", label: "80" }, { id: "100", label: "100" }]
                                current: String(veyra.preferences.guestBitrateMbps || 50)
                                onPicked: id => veyra.setPreference("guestBitrateMbps", Number(id))
                            }
                        }
                        UiRow {
                            id: reachRow
                            // The broker's host (GuestShareModel.brokerHost), "" when this build has none,
                            // and where a guest's relay comes from when the direct path fails.
                            readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
                            readonly property string host: g ? g.brokerHost : ""
                            readonly property string reach: host.length > 0 && veyra.preferences.guestReach !== "tailnet" ? "broker" : "tailnet"
                            readonly property string relay: g ? g.relay : "none"
                            title: qsTr("accès")
                            desc: reach === "broker"
                                  ? qsTr("lien https sur %1 : ton ami n'installe rien, l'image va de ton pc au sien.").arg(host) + " "
                                    + (relay === "owner" ? qsTr("si le direct échoue, le relais cloudflare prend la suite, dans sa limite du mois.")
                                       : relay === "key" ? qsTr("si le direct échoue, ta clé turn cloudflare sert de relais.")
                                       : qsTr("pas de relais : si vos deux box bloquent le direct, la connexion ne passe pas."))
                                  : qsTr("ton tailnet : ton ami installe tailscale et tu lui partages ce pc. l'image ne passe que par tailscale.")
                            UiSeg {
                                options: reachRow.host.length > 0 ? [{ id: "broker", label: reachRow.host }, { id: "tailnet", label: qsTr("tailnet") }]
                                                                  : [{ id: "tailnet", label: qsTr("tailnet") }]
                                current: reachRow.reach
                                onPicked: id => veyra.setPreference("guestReach", id)
                            }
                        }
                        UiRow {
                            title: qsTr("la manette")
                            desc: qsTr("qui joue quand un invité est là. modifiable pendant la partie, depuis « partager ».")
                            UiSeg {
                                options: [{ id: "guest", label: qsTr("l'invité") }, { id: "together", label: qsTr("à deux") }, { id: "watch", label: qsTr("moi seul") }]
                                current: veyra.preferences.guestMode || "guest"
                                onPicked: id => veyra.setPreference("guestMode", id)
                            }
                        }
                    }
                    // Several friends on one share (RoomModel.h): who may play, turns, dice.
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("invités à la fois")
                            desc: qsTr("une seule manette arrive à la ps5 : les autres regardent en direct, avec le son. chacun reçoit l'image que sa connexion supporte ; ton envoi max est partagé entre eux.")
                            UiSeg {
                                options: [{ id: "1", label: "1" }, { id: "2", label: "2" }, { id: "3", label: "3" }, { id: "4", label: "4" }, { id: "6", label: "6" }]
                                current: String(veyra.preferences.guestMaxViewers || 4)
                                onPicked: id => veyra.setPreference("guestMaxViewers", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("les nouveaux venus")
                            desc: qsTr("peuvent prendre la manette (file d'attente, dés), ou regardent seulement. tu changes chacun depuis « partager ».")
                            UiSeg {
                                options: [{ id: "player", label: qsTr("peuvent jouer") }, { id: "watcher", label: qsTr("regardent") }]
                                current: veyra.preferences.guestDefaultSeat || "player"
                                onPicked: id => veyra.setPreference("guestDefaultSeat", id)
                            }
                        }
                        UiRow {
                            title: qsTr("tour de manette")
                            desc: qsTr("quand quelqu'un attend, la manette passe au suivant après ce temps.")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("non") }, { id: "15", label: "15 min" }, { id: "30", label: "30 min" }, { id: "60", label: "1 h" }]
                                current: String(veyra.preferences.guestTurnMinutes || 0)
                                onPicked: id => veyra.setPreference("guestTurnMinutes", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("les dés")
                            desc: qsTr("un invité peut lancer les dés pour la manette. ce pc tire les dés, tout le monde voit le lancer, le gagnant joue.")
                            UiSwitch { on: veyra.preferences.guestDice !== false; onSwitched: v => veyra.setPreference("guestDice", v) }
                        }
                        UiRow {
                            title: qsTr("priorité à toi")
                            desc: qsTr("quand tu appuies sur ta propre manette, tu reprends la main ; l'invité la retrouve après deux minutes sans toi.")
                            UiSwitch { on: veyra.preferences.guestOwnerPriority !== false; onSwitched: v => veyra.setPreference("guestOwnerPriority", v) }
                        }
                        UiRow {
                            title: qsTr("historique visible par les invités")
                            desc: qsTr("leur page montre les dernières parties : qui, quand, combien de temps.")
                            UiSwitch { on: veyra.preferences.guestHistoryToGuests === true; onSwitched: v => veyra.setPreference("guestHistoryToGuests", v) }
                        }
                    }

                    // Discord (GuestNotify.h): the webhook stays sealed on this PC, never shown again.
                    UiCard {
                        id: discordCard
                        readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
                        readonly property var d: g ? g.discord : ({})
                        readonly property var ev: d.events || ({})
                        UiRow {
                            first: true
                            title: qsTr("discord")
                            desc: discordCard.d.configured ? qsTr("webhook configuré") + (discordCard.d.status ? " · " + discordCard.d.status : "")
                                                           : qsTr("colle le lien d'un webhook de ton salon (paramètres du salon → intégrations → webhooks). il reste chiffré sur ce pc.")
                            UiField {
                                id: hookField
                                visible: !discordCard.d.configured
                                implicitWidth: 220
                                fieldHeight: 32
                                echoMode: TextInput.Password
                                placeholderText: "https://discord.com/api/webhooks/…"
                            }
                            UiButton {
                                visible: !discordCard.d.configured
                                text: qsTr("enregistrer")
                                size: 32
                                enabled: hookField.text.length > 0
                                onClicked: if (discordCard.g && discordCard.g.setDiscordWebhook(hookField.text)) hookField.text = ""
                            }
                            UiButton { visible: discordCard.d.configured === true; text: qsTr("tester"); size: 32; onClicked: discordCard.g.testDiscord() }
                            UiButton { visible: discordCard.d.configured === true; text: qsTr("retirer"); size: 32; onClicked: discordCard.g.clearDiscordWebhook() }
                        }
                        Repeater {
                            model: [{ id: "free", label: qsTr("ps5 libre") }, { id: "joined", label: qsTr("quelqu'un se connecte") },
                                    { id: "left", label: qsTr("quelqu'un part") }, { id: "pad", label: qsTr("quelqu'un a la manette") },
                                    { id: "queue", label: qsTr("file d'attente") }, { id: "dice", label: qsTr("résultat des dés") },
                                    { id: "share-started", label: qsTr("partage ouvert") }, { id: "share-stopped", label: qsTr("partage arrêté") },
                                    { id: "share-stuck", label: qsTr("partage bloqué") }]
                            delegate: UiRow {
                                required property var modelData
                                visible: discordCard.d.configured === true
                                title: modelData.label
                                UiSwitch { on: discordCard.ev[modelData.id] === true; onSwitched: v => discordCard.g.setDiscordEvent(modelData.id, v) }
                            }
                        }
                    }

                    // Who held the controller, when, how long (guest-plays.dpapi).
                    UiCard {
                        id: playsCard
                        readonly property var g: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
                        readonly property var list: g ? g.plays : []
                        function length(s) {
                            if (s < 60) return s + " s"
                            const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60)
                            return h > 0 ? h + " h " + (m < 10 ? "0" : "") + m : m + " min"
                        }
                        UiRow {
                            first: true
                            title: qsTr("historique des parties")
                            desc: playsCard.list.length > 0 ? qsTr("%1 parties").arg(playsCard.list.length) : qsTr("personne n'a encore joué")
                            UiButton { visible: playsCard.list.length > 0; text: qsTr("effacer"); size: 32; onClicked: playsCard.g.clearPlays() }
                        }
                        Repeater {
                            model: playsCard.list.slice(0, 12)
                            delegate: UiRow {
                                required property var modelData
                                title: modelData.name
                                desc: Qt.formatDateTime(new Date(modelData.at), "dd/MM HH:mm")
                                Text { text: playsCard.length(modelData.seconds); color: Theme.t2; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("le lien de partage change à chaque partage et quand tu le renouvelles. le navigateur ne transmet ni le gyroscope, ni le pavé tactile (sauf son clic), ni les gâchettes adaptatives. les conditions de sony ne prévoient pas qu'une autre personne joue à distance sur ton compte.")
                        color: Theme.t3
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsMeta
                        lineHeight: 1.4
                        wrapMode: Text.Wrap
                    }
                }

                // invités -------------------------------------------------------------
                // Friends with their own link (vyAccess, GuestAccessModel.h): the gateway, the
                // accesses, the visit in progress and the last ones.
                ColumnLayout {
                    id: invitesSection
                    visible: root.section === "invites"
                    Layout.fillWidth: true
                    spacing: 24
                    readonly property var a: typeof vyAccess !== "undefined" && vyAccess ? vyAccess : null
                    property int draftModes: 1
                    property string draftAsk: "ask"
                    property string draftFeed: "120"
                    property string draftDays: "0"
                    property string confirmRevoke: ""
                    function when(ms) {
                        const d = new Date(ms), now = new Date()
                        const today = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime()
                        const t = Qt.formatTime(d, "HH:mm")
                        if (ms >= today) return qsTr("aujourd'hui %1").arg(t)
                        if (ms >= today - 86400000) return qsTr("hier %1").arg(t)
                        return Qt.formatDate(d, "dd/MM") + " " + t
                    }
                    function length(s) {
                        if (s <= 0) return ""
                        if (s < 60) return s + " s"
                        const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60)
                        return h > 0 ? h + " h " + (m < 10 ? "0" : "") + m : m + " min"
                    }
                    function summary(x) {
                        const busy = (x.modes & 2) ? qsTr("à deux si tu joues") : (x.modes & 4) ? qsTr("regarde si tu joues") : qsTr("occupé si tu joues")
                        const feed = x.feed === "real" ? qsTr("images réelles") : x.feed === "60" ? qsTr("fluide 60") : qsTr("fluide 120")
                        const end = x.expired ? qsTr("expiré") : x.expires > 0 ? qsTr("jusqu'au %1").arg(Qt.formatDate(new Date(x.expires), "dd/MM")) : qsTr("sans fin")
                        const hours = x.from.length > 0 ? x.from + "–" + x.to : ""
                        return [x.ask ? qsTr("demande ton accord") : qsTr("entre sans demander"), busy, feed, end, hours].filter(s => s.length).join(" · ")
                    }

                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("accepter les invités en mon absence")
                            desc: qsTr("BooVI garde l'accès ouvert même sans stream : une icône reste près de l'horloge, et réduire la fenêtre l'y range. ton ami réveille la ps5 avec son lien.")
                            UiSwitch { on: invitesSection.a ? invitesSection.a.gateway : false; onSwitched: v => invitesSection.a.setGateway(v) }
                        }
                        UiRow {
                            title: qsTr("démarrer avec windows")
                            desc: qsTr("BooVI s'ouvre dans la zone de notification à l'ouverture de ta session, prêt à accueillir.")
                            UiSwitch { on: invitesSection.a ? invitesSection.a.startWithWindows : false; onSwitched: v => invitesSection.a.setStartWithWindows(v) }
                        }
                        UiRow {
                            title: qsTr("ton nom pour tes invités")
                            desc: qsTr("ce que sa page affiche : « en attente de l'accord de … ».")
                            UiField {
                                implicitWidth: 180
                                fieldHeight: 32
                                maximumLength: 24
                                placeholderText: qsTr("ton prénom")
                                text: veyra.preferences.gatewayHostName || ""
                                onEdited: t => veyra.setPreference("gatewayHostName", t)
                            }
                        }
                        UiRow {
                            title: qsTr("remettre la ps5 en veille")
                            desc: qsTr("quand c'est la visite qui a réveillé la console, après le départ de ton ami.")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("non") }, { id: "5", label: "5 min" }, { id: "10", label: "10 min" }, { id: "30", label: "30 min" }]
                                current: String(veyra.preferences.gatewayRestMinutes !== undefined ? veyra.preferences.gatewayRestMinutes : 10)
                                onPicked: id => veyra.setPreference("gatewayRestMinutes", Number(id))
                            }
                        }
                        UiRow {
                            title: qsTr("invité inactif")
                            desc: qsTr("la visite se termine quand sa manette ne bouge plus depuis…")
                            UiSeg {
                                options: [{ id: "10", label: "10 min" }, { id: "20", label: "20 min" }, { id: "60", label: "1 h" }, { id: "0", label: qsTr("jamais") }]
                                current: String(veyra.preferences.gatewayIdleMinutes !== undefined ? veyra.preferences.gatewayIdleMinutes : 20)
                                onPicked: id => veyra.setPreference("gatewayIdleMinutes", Number(id))
                            }
                        }
                    }

                    UiCard {
                        visible: invitesSection.a !== null && Object.keys(invitesSection.a.visit).length > 0
                        UiRow {
                            first: true
                            readonly property var v: invitesSection.a ? invitesSection.a.visit : ({})
                            title: v.phase === "playing" ? qsTr("%1 joue").arg(v.name) : v.phase === "asking" ? qsTr("%1 attend ton accord").arg(v.name) : qsTr("%1 arrive").arg(v.name)
                            desc: v.phase === "waking" ? qsTr("réveil de la ps5…") : v.phase === "connecting" ? qsTr("connexion…")
                                : v.phase === "playing" ? qsTr("depuis %1").arg(Qt.formatTime(new Date(v.since), "HH:mm")) + (v.friends && v.friends.length > 1 ? " · " + v.friends.join(", ") : "") : ""
                            UiButton { visible: invitesSection.a && invitesSection.a.pending.name !== undefined; text: qsTr("accepter"); kind: "solid"; size: 32; focusPolicy: Qt.NoFocus; onClicked: invitesSection.a.approve() }
                            UiButton { visible: invitesSection.a && invitesSection.a.pending.name !== undefined; text: qsTr("refuser"); size: 32; onClicked: invitesSection.a.refuse() }
                            UiButton { visible: invitesSection.a && invitesSection.a.pending.name === undefined; text: qsTr("couper"); size: 32; onClicked: invitesSection.a.endVisit() }
                        }
                    }

                    UiCard {
                        visible: invitesSection.a !== null && invitesSection.a.newLink.length > 0
                        UiRow {
                            first: true
                            title: qsTr("le lien de %1").arg(invitesSection.a ? invitesSection.a.newLinkName : "")
                            desc: qsTr("il ne s'affiche qu'une fois : envoie-le en privé. il ouvre la page dans un navigateur, ou se colle dans « rejoindre un partage » de BooVI.")
                            UiButton { text: qsTr("copier"); kind: "solid"; size: 32; onClicked: invitesSection.a.copyNewLink() }
                            UiButton { text: qsTr("c'est fait"); size: 32; onClicked: invitesSection.a.dismissNewLink() }
                        }
                    }

                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("inviter un ami")
                            desc: qsTr("un lien personnel qui dure. tu peux le couper quand tu veux.")
                            UiField { id: draftName; implicitWidth: 180; fieldHeight: 32; maximumLength: 24; placeholderText: qsTr("son prénom") }
                        }
                        UiRow {
                            title: qsTr("quand il se connecte")
                            UiSeg {
                                options: [{ id: "ask", label: qsTr("demander mon accord") }, { id: "auto", label: qsTr("autoriser automatiquement") }]
                                current: invitesSection.draftAsk
                                onPicked: id => invitesSection.draftAsk = id
                            }
                        }
                        UiRow {
                            title: qsTr("si tu joues déjà")
                            desc: qsTr("seul à la console, il joue. si tu es en jeu, tu reçois toujours une demande.")
                            UiSeg {
                                options: [{ id: "1", label: qsTr("occupé") }, { id: "3", label: qsTr("à deux") }, { id: "5", label: qsTr("regarder") }]
                                current: String(invitesSection.draftModes)
                                onPicked: id => invitesSection.draftModes = Number(id)
                            }
                        }
                        UiRow {
                            title: qsTr("ce qu'il reçoit au plus")
                            UiSeg {
                                options: [{ id: "real", label: qsTr("images réelles") }, { id: "60", label: qsTr("fluide 60") }, { id: "120", label: qsTr("fluide 120") }]
                                current: invitesSection.draftFeed
                                onPicked: id => invitesSection.draftFeed = id
                            }
                        }
                        UiRow {
                            title: qsTr("durée")
                            UiSeg {
                                options: [{ id: "0", label: qsTr("sans fin") }, { id: "1", label: qsTr("1 jour") }, { id: "7", label: qsTr("1 semaine") }, { id: "30", label: qsTr("1 mois") }]
                                current: invitesSection.draftDays
                                onPicked: id => invitesSection.draftDays = id
                            }
                        }
                        UiRow {
                            title: qsTr("heures permises")
                            desc: qsTr("vide : à toute heure. 22:00 à 02:00 passe minuit.")
                            UiField { id: draftFrom; implicitWidth: 76; fieldHeight: 32; maximumLength: 5; placeholderText: "18:00" }
                            UiField { id: draftTo; implicitWidth: 76; fieldHeight: 32; maximumLength: 5; placeholderText: "23:00" }
                        }
                        UiRow {
                            title: qsTr("les conditions de sony ne prévoient pas qu'une autre personne joue à distance sur ton compte.")
                            descColor: Theme.t3
                            UiButton {
                                text: qsTr("créer le lien")
                                kind: "solid"
                                size: 32
                                enabled: draftName.text.trim().length > 0
                                onClicked: {
                                    invitesSection.a.create(draftName.text, invitesSection.draftAsk === "ask", invitesSection.draftModes, invitesSection.draftFeed,
                                                            Number(invitesSection.draftDays), draftFrom.text.trim(), draftTo.text.trim())
                                    draftName.text = ""
                                }
                            }
                        }
                    }

                    UiCard {
                        visible: invitesSection.a !== null && invitesSection.a.accesses.length > 0
                        Repeater {
                            model: invitesSection.a ? invitesSection.a.accesses : []
                            delegate: UiRow {
                                required property var modelData
                                required property int index
                                first: index === 0
                                title: modelData.name
                                desc: invitesSection.summary(modelData)
                                descColor: modelData.expired ? Theme.t3 : Theme.t2
                                UiButton { text: qsTr("nouveau lien"); size: 32; onClicked: invitesSection.a.regenerate(modelData.id) }
                                UiButton {
                                    text: invitesSection.confirmRevoke === modelData.id ? qsTr("confirmer") : qsTr("révoquer")
                                    size: 32
                                    onClicked: {
                                        if (invitesSection.confirmRevoke !== modelData.id) { invitesSection.confirmRevoke = modelData.id; return }
                                        invitesSection.confirmRevoke = ""
                                        invitesSection.a.revoke(modelData.id)
                                    }
                                }
                            }
                        }
                    }

                    UiCard {
                        visible: invitesSection.a !== null && invitesSection.a.history.length > 0
                        Repeater {
                            model: invitesSection.a ? invitesSection.a.history.slice(0, 8) : []
                            delegate: UiRow {
                                required property var modelData
                                required property int index
                                first: index === 0
                                title: [modelData.name, invitesSection.when(modelData.start), invitesSection.length(modelData.seconds)].filter(s => s.length).join(" · ")
                                desc: modelData.outcome
                                UiButton { visible: index === 0; text: qsTr("effacer"); size: 32; onClicked: invitesSection.a.clearHistory() }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("ta ps5, son appairage et son code de connexion restent sur ce pc : ton ami ne reçoit que l'image, le son et sa manette. un seul invité à la fois ; un lien révoqué ne marche plus, tout de suite.")
                        color: Theme.t3
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsMeta
                        lineHeight: 1.4
                        wrapMode: Text.Wrap
                    }
                }

                // app -------------------------------------------------------------------
                ColumnLayout {
                    id: appSection
                    visible: root.section === "app"
                    Layout.fillWidth: true
                    spacing: 24
                    UiCard {
                        UiRow {
                            first: true
                            title: qsTr("couleurs")
                            UiSeg {
                                options: [{ id: "sage", label: qsTr("violet") }, { id: "dusk", label: qsTr("crépuscule") }, { id: "classic", label: qsTr("neutre") }]
                                current: veyra.preferences.palette || "sage"
                                onPicked: id => veyra.setPreference("palette", id)
                            }
                        }
                        UiRow {
                            title: qsTr("animations réduites")
                            desc: qsTr("coupe les mouvements de l'interface.")
                            UiSwitch { on: veyra.reducedMotion; onSwitched: v => veyra.reducedMotion = v }
                        }
                        UiRow {
                            title: qsTr("langue")
                            UiSelect {
                                implicitWidth: 180
                                options: [{ id: "auto", label: qsTr("celle de windows") }, { id: "fr", label: "français" }, { id: "en", label: "english" },
                                          { id: "zh-CN", label: "简体中文" }, { id: "zh-TW", label: "繁體中文" }, { id: "ja", label: "日本語" }]
                                current: veyra.preferences.language || "auto"
                                onPicked: id => veyra.setPreference("language", id)
                            }
                        }
                        UiRow {
                            title: qsTr("journaux")
                            desc: qsTr("utile pour signaler un problème.")
                            UiButton { text: qsTr("ouvrir le dossier"); size: 32; onClicked: veyra.openLogFolder() }
                        }
                    }
                }
            }
        }
    }

    QtObject { id: swap; signal enter() }
    onSectionEntered: swap.enter()
    UiEnter { target: heading; page: swap; order: 0; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: imageSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: latencySection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: displaySection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: streamingSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: audioSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: padSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: kbmSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: capturesSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: shareSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: invitesSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
    UiEnter { target: appSection; page: swap; order: 1; stagger: 60; dy: 6; duration: 320 }
}
