// In game (Stream mockup): the picture is full-bleed and everything here floats over it. A
// stats card top right (or a compact fps pill), the pad and keyboard/mouse chips top left, the "pad paused" card in
// the bottom-right corner when BooVI loses the focus, and the toolbar at the bottom while the pointer
// moves, and the keyboard/mouse bindings card while Ctrl+M holds it open. Every floating surface is a videoCover, so main.cpp cuts it out of the native
// video window and it shows above the picture.
// `preview` renders all of it without a session (--page stream-preview): sample numbers,
// no engine call, so the layout can be checked while the console is in use elsewhere.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root
    property bool preview: false
    property bool previewPaused: false
    property bool previewKbm: false
    property bool previewNoPad: false   // --page stream-preview-nopad: no pad, with the hint
    // The bindings card (Ctrl+M during a PS5 stream; --page stream-preview-keys shows it).
    property bool keysOpen: false
    // Main.qml: true while the pointer moved recently (and always in preview).
    property bool controlsActive: false
    property bool fullscreen: false
    signal requestPage(string page)
    signal requestFullscreen()
    readonly property bool toolbarHovered: toolHover.hovered

    Sources { id: sources }
    StreamPresets { id: presets }
    readonly property string kind: preview ? "ps5" : sources.live
    readonly property var ps5: veyra.ps5 || ({})
    readonly property var stream: kind === "pc" ? (sources.pc.stream || ({})) : kind === "xbox" ? (sources.xbox.stream || ({}))
                                : kind === "join" ? (sources.join.stream || ({})) : ({})
    readonly property bool kbmOn: preview ? previewKbm : kind === "ps5" || kind === "join" ? ps5.kbm === true : kind === "pc" ? veyra.moonlightCaptured : false
    readonly property bool kbmAvailable: kind === "ps5" ? ps5.viewOnly !== true : kind === "pc" || kind === "join"
    readonly property bool paused: preview ? previewPaused : kind === "ps5" && ps5.inputPaused === true
    // Stats card: "full", a compact fps pill, or "off"; opacity 30-100 %. Saved preferences.
    readonly property string statsMode: {
        const m = veyra.preferences.streamStatsMode
        if (m === "full" || m === "compact" || m === "off") return m
        return veyra.preferences.streamStats === false ? "off" : "full"
    }
    readonly property real statsOpacity: {
        const v = Number(veyra.preferences.streamStatsOpacity)
        return v >= 30 && v <= 100 ? v / 100 : 1
    }
    property string lastVisibleMode: "full"
    readonly property bool statsOn: kind.length > 0 && statsMode === "full"
    readonly property bool pillOn: kind.length > 0 && statsMode === "compact"
    readonly property bool detailed: veyra.preferences.streamStatsDetailed === true
    readonly property int screenHeight: Math.round(Screen.height * Screen.devicePixelRatio)
    visible: kind.length > 0

    function setStatsMode(m) {
        if (m !== "off") lastVisibleMode = m
        veyra.setPreference("streamStatsMode", m)
    }
    function toggleStats() { setStatsMode(statsMode === "off" ? lastVisibleMode : "off") }
    // 100 -> 70 -> 45 -> 100 %.
    function cycleStatsOpacity() {
        const next = statsOpacity > 0.85 ? 70 : statsOpacity > 0.55 ? 45 : 100
        veyra.setPreference("streamStatsOpacity", next)
    }
    function toggleKbm() {
        if (preview) { previewKbm = !previewKbm; return }
        if (kind === "ps5" || kind === "join") veyra.ps5Kbm(!kbmOn)
        else if (kind === "pc") veyra.moonlightCapture(!kbmOn)
    }
    function toggleKeys() { if (kind === "ps5") keysOpen = !keysOpen }
    // Ctrl+M: from the capture while it holds the keyboard (the key never reaches the PS5),
    // from this shortcut otherwise.
    Connections {
        target: veyra
        function onPs5KeysRequested() { root.toggleKeys() }
    }
    Shortcut {
        sequence: "Ctrl+M"
        enabled: root.kind === "ps5" && !root.kbmOn
        onActivated: root.toggleKeys()
    }
    onKindChanged: if (kind !== "ps5") keysOpen = false

    function fps(v) { return v > 0 ? Math.round(v) + qsTr(" i/s") : "—" }

    // Numbers on the card, by source. A value the source does not report is absent, never 0.
    readonly property real receivedFps: preview ? 60 : (kind === "pc" || kind === "xbox" || kind === "join") ? (stream.receivedFps || 0) : (veyra.captureFps > 0 ? veyra.captureFps : veyra.sourceFps)
    // Camera anticipation on a joined share (1 alone, 2 with interpolation): what it does now and
    // how sure the learnt model is. Alone, the client runs without frame generation whatever the preset says.
    readonly property int anticipationMode: kind === "join" ? (stream.anticipationMode || 0) : 0
    readonly property string anticipationState: !stream.anticipationRunning ? qsTr("inactive")
        : (stream.anticipationLearnt ? (stream.anticipationActive ? qsTr("active") : qsTr("prête")) : qsTr("apprend"))
          + " · " + qsTr("confiance %1").arg((stream.anticipationConfidence || 0).toFixed(2).replace(".", ","))
          + (stream.anticipationLearnt ? " · " + qsTr("gain appris") : "")
    // A joined share with camera anticipation: the screen gets a draw every refresh, not only the stream's pictures.
    readonly property real shownFps: preview ? presets.outputFps(60)
        : kind === "join" && stream.anticipationRunning && stream.anticipationFps > 0 ? stream.anticipationFps
        : veyra.submitFpsKnown ? veyra.submitFps : 0
    function ms(v) { return v.toFixed(1).replace(".", ",") + " ms" }
    // What the engine measures of the delay, and what the game really renders. The delay runs from
    // the decode of a real picture to its Present, plus the screen's scan-out when DXGI reports it;
    // the network and the console's encoder are before the first point this PC can time.
    readonly property real contentFps: preview ? 30 : veyra.contentFps
    readonly property real delayMs: preview ? 31.5 : veyra.liveDelayMs > 0 ? veyra.liveDelayMs + Math.max(0, veyra.scanoutDelayMs) : 0
    readonly property real gpuMs: preview ? 6.1 : veyra.chainTotalMsKnown ? veyra.chainTotalMs : 0
    readonly property var delayCells: {
        const out = []
        if (contentFps > 0) out.push({ k: qsTr("images réelles"), v: fps(contentFps),
            tip: qsTr("images différentes par seconde dans le flux. un jeu à 30 i/s arrive en double dans un flux à 60 : les doublons sont retirés avant la frame gen.") })
        if (delayMs > 0) out.push({ k: qsTr("délai total"), v: ms(delayMs),
            tip: (preview || veyra.scanoutDelayMs >= 0
                  ? qsTr("du décodage d'une vraie image à son affichage : attente de la frame gen, calcul gpu, présentation et balayage de l'écran.")
                  : qsTr("du décodage d'une vraie image à sa présentation : attente de la frame gen et calcul gpu. le balayage de l'écran n'est pas mesuré ici."))
                 + " " + qsTr("le réseau et l'encodage par la console ne sont pas comptés.") })
        if (gpuMs > 0) out.push({ k: qsTr("délai gpu"), v: ms(gpuMs),
            tip: qsTr("temps de calcul gpu de la chaîne (super résolution, frame gen, présentation) pour une image reçue.") })
        return out
    }
    readonly property var cells: {
        const out = [{ k: qsTr("reçu"), v: fps(receivedFps) }].concat(delayCells.slice(0, 1),
            [{ k: qsTr("affiché"), v: fps(shownFps) }], delayCells.slice(1))
        if (preview) return out.concat([{ k: qsTr("débit"), v: "11,2 Mb/s" }, { k: qsTr("perte"), v: "0" }])
        if (kind === "pc" || kind === "xbox" || kind === "join") {
            out.push({ k: qsTr("débit"), v: stream.videoMbps > 0 ? stream.videoMbps.toFixed(1).replace(".", ",") + " Mb/s" : "—" })
            if (kind === "join") out.push({ k: qsTr("perte"), v: (stream.lossPercent || 0).toFixed(1).replace(".", ",") + " %", warn: (stream.lossPercent || 0) >= 1 })
            else out.push({ k: qsTr("perte"), v: String(kind === "pc" ? (stream.lost || 0) : (stream.dropped || 0)), warn: (stream.lost || 0) > 0 })
            if (stream.rttMs > 0) out.push({ k: qsTr("réseau"), v: stream.rttMs.toFixed(1).replace(".", ",") + " ms" })
            if (stream.decodeMs > 0) out.push({ k: qsTr("décodage"), v: stream.decodeMs.toFixed(1).replace(".", ",") + " ms", warn: softwareDecode,
                tip: softwareDecode ? qsTr("décodage logiciel sur le processeur : le décodeur matériel du gpu n'a pas pu être utilisé.") : "" })
            // A joined share: the friend's picture, from its decode on the host to this screen.
            if (kind === "join" && stream.hostToScreenMs > 0) out.push({ k: qsTr("hôte → écran"), v: stream.hostToScreenMs.toFixed(1).replace(".", ",") + " ms",
                tip: qsTr("d'une image décodée chez l'hôte à son affichage ici : encodage, réseau, décodage, frame gen et présentation. horloges synchronisées.") })
        } else if (kind === "capture") {
            out.push({ k: qsTr("perdues"), v: String(Math.round(veyra.captureDropped)), warn: veyra.captureDropped > 0 })
        }
        return out
    }
    // A joined share names its decoder and the GPU it runs on: "h.264 · d3d12va · amd".
    readonly property bool softwareDecode: kind === "join" && String(stream.decoder || "").startsWith("software")
    readonly property string decoderText: kind !== "join" || !stream.decoder ? ""
        : [softwareDecode ? qsTr("logiciel") : String(stream.decoder), String(stream.gpu || "")].filter(x => x.length).join(" · ")
    readonly property string codec: kind === "ps5" ? ["h.264", "h.265", "h.265 hdr"][ps5.codec || 0]
                                  : [String(stream.codec || "").toLowerCase(), decoderText].filter(x => x.length).join(" · ")
    readonly property var bars: {
        const src = preview ? [16.6, 16.8, 16.5, 17.1, 16.6, 16.4, 16.9, 16.6, 18.2, 16.6, 16.5, 16.7, 16.6, 16.6, 17.4, 16.5, 16.6, 16.8, 16.6, 16.5, 16.7, 16.6, 16.9, 16.6]
                            : (veyra.frameTimes || []).slice(-24)
        if (src.length === 0) return []
        const sorted = src.slice().sort((a, b) => a - b)
        const median = sorted[Math.floor(sorted.length / 2)] || 1
        return src.map(v => Math.max(0.15, Math.min(1, v / (median * 1.6))))
    }

    // --- stats card ----------------------------------------------------------------------
    Rectangle {
        id: stats
        objectName: "stream-stats"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 24
        width: 240
        height: statsCol.implicitHeight + 32
        radius: Theme.rLg
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        visible: opacity > 0.01
        // Hovered: full strength, so the controls stay readable at any opacity.
        opacity: root.statsOn ? (statsHover.hovered ? 1 : root.statsOpacity) : 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        HoverHandler { id: statsHover }
        ColumnLayout {
            id: statsCol
            x: 16
            y: 16
            width: parent.width - 32
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: (sources.shortName(root.kind) || "").toUpperCase() + qsTr(" · EN DIRECT")
                    color: Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsLabel
                    font.letterSpacing: 0.8
                }
                Row {
                    spacing: 2
                    opacity: statsHover.hovered ? 1 : 0
                    visible: opacity > 0.01
                    Behavior on opacity { NumberAnimation { duration: Theme.d(150) } }
                    UiButton { kind: "tool"; size: 24; iconSize: 14; iconName: "minus"; tip: qsTr("réduire"); onClicked: root.setStatsMode("compact") }
                    UiButton { kind: "tool"; size: 24; iconSize: 14; iconName: "sun"; tip: qsTr("opacité · %1 %").arg(Math.round(root.statsOpacity * 100)); onClicked: root.cycleStatsOpacity() }
                    UiButton { kind: "tool"; size: 24; iconSize: 14; iconName: "eyeoff"; tip: qsTr("masquer"); onClicked: root.setStatsMode("off") }
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                rowSpacing: 12
                columnSpacing: 16
                Repeater {
                    model: root.cells
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        spacing: 4
                        HoverHandler { id: cellHover }
                        ToolTip.visible: (modelData.tip || "").length > 0 && cellHover.hovered
                        ToolTip.delay: 600
                        ToolTip.text: modelData.tip || ""
                        Text { text: modelData.k; color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                        Text {
                            text: modelData.v
                            color: modelData.warn ? Theme.warn : Theme.t1
                            font.family: Theme.fontMono
                            font.pixelSize: 16
                            font.features: { "tnum": 1 }
                            Behavior on color { ColorAnimation { duration: Theme.d(150) } }
                        }
                    }
                }
            }
            Row {
                visible: root.bars.length > 0
                Layout.fillWidth: true
                height: 16
                spacing: 2
                Repeater {
                    model: root.bars.length
                    delegate: Item {
                        required property int index
                        width: (statsCol.width - 2 * (root.bars.length - 1)) / root.bars.length
                        height: 16
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 16 * (root.bars[index] || 0)
                            color: Theme.t2
                            opacity: 0.3 + (index / root.bars.length) * 0.7
                            Behavior on height { NumberAnimation { duration: Theme.d(500); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: [root.codec, root.anticipationMode === 1 ? presets.summary(presets.current).replace(/ · fg x\d+/, "") : presets.summary(presets.current)]
                    .filter(x => x.length).join(" · ")
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                elide: Text.ElideRight
            }
            Text {
                objectName: "stats-anticipation"
                Layout.fillWidth: true
                visible: root.anticipationMode > 0
                text: qsTr("anticipation : %1").arg(root.anticipationState)
                color: stream.anticipationActive ? Theme.accentText : Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                wrapMode: Text.WordWrap
                HoverHandler { id: anticipationHover }
                ToolTip.visible: anticipationHover.hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("la caméra est redessinée à chaque rafraîchissement de l'écran selon le stick droit, sans attendre l'image suivante. délai appris : %1 ms. seule la caméra est anticipée : hud, personnages et tirs suivent le flux comme avant.").arg(Math.round(stream.anticipationLagMs || 0))
            }
            Text {
                Layout.fillWidth: true
                visible: presets.fg && root.anticipationMode !== 1
                text: qsTr("sortie visée · %1").arg(root.fps(presets.outputFps(veyra.sourceFps)))
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
            }
            ColumnLayout {
                visible: root.detailed
                Layout.fillWidth: true
                spacing: 4
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
                Repeater {
                    model: root.preview ? [{ label: "sr", ms: 1.8, measured: true }, { label: "fg", ms: 2.6, measured: true }, { label: qsTr("présentation"), ms: 0.4, measured: true }]
                                        : (veyra.stageTimings || [])
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: String(modelData.label).toLowerCase(); color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta; elide: Text.ElideRight }
                        Text { text: modelData.measured ? modelData.ms.toFixed(2).replace(".", ",") + " ms" : "—"; color: Theme.t2; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta; font.features: { "tnum": 1 } }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.decoderText.length > 0
                    Text { Layout.fillWidth: true; text: qsTr("décodeur"); color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                    Text { text: String(root.stream.decoder) + " · " + String(root.stream.gpu || "?"); color: root.softwareDecode ? Theme.warn : Theme.t2; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.preview || veyra.chainTotalMsKnown
                    Text { Layout.fillWidth: true; text: qsTr("total gpu"); color: Theme.t2; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
                    Text { text: (root.preview ? 4.8 : veyra.chainTotalMs).toFixed(2).replace(".", ",") + " ms"; color: Theme.accentText; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta; font.features: { "tnum": 1 } }
                }
            }
        }
    }

    // --- compact fps pill (stats reduced) ---------------------------------------------------
    Rectangle {
        id: pill
        objectName: "stream-stats-pill"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rMd
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 24
        width: pillRow.implicitWidth + 24
        height: 32
        radius: Theme.rMd
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        visible: opacity > 0.01
        opacity: root.pillOn ? (pillHover.hovered ? 1 : root.statsOpacity) : 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(200); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        HoverHandler { id: pillHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.setStatsMode("full") }
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: (root.receivedFps > 0 ? Math.round(root.receivedFps) : "—") + " →"
                color: Theme.t3
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                font.features: { "tnum": 1 }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.fps(root.shownFps)
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                font.features: { "tnum": 1 }
            }
        }
    }

    // --- chips ---------------------------------------------------------------------------
    Column {
        id: chips
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 24
        spacing: 8
        Rectangle {
            id: padChip
            readonly property bool videoCover: true
            property real coverRadius: Theme.rMd
            visible: root.kind === "ps5" || root.kind === "pc" || root.kind === "xbox" || root.kind === "join"
            anchors.left: parent.left
            width: padRow.implicitWidth + 24
            height: 32
            radius: Theme.rMd
            color: Theme.card
            border.width: 1
            border.color: root.ps5.viewOnly === true ? Theme.warn : Theme.stroke
            Row {
                id: padRow
                anchors.centerIn: parent
                spacing: 8
                VIcon { anchors.verticalCenter: parent.verticalCenter; name: "pad"; size: 14; strokeWidth: 1.5; color: Theme.accentText }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.preview ? (root.previewNoPad ? qsTr("aucune manette") : "dualsense · usb")
                        : root.kind === "ps5" && root.ps5.viewOnly === true ? qsTr("regarder seulement")
                        : root.ps5.controller === true ? String(root.ps5.pad || qsTr("manette")).toLowerCase() : qsTr("aucune manette")
                    color: root.ps5.viewOnly === true && !root.preview ? Theme.warn : Theme.t1
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                }
            }
        }
        PadNotice {
            hint: (root.preview ? root.previewNoPad : root.ps5.padHint === true) && !root.paused
            padName: root.ps5.pad || ""
            arrivals: root.ps5.padArrivals || 0
            onCloseRequested: if (root.preview) root.previewNoPad = false; else veyra.ps5PadHintClose()
        }
        Rectangle {
            id: kbmChip
            readonly property bool videoCover: true
            property real coverRadius: Theme.rMd
            anchors.left: parent.left
            width: kbmChipText.implicitWidth + 24
            height: 32
            radius: Theme.rMd
            color: Theme.card
            border.width: 1
            border.color: Theme.stroke2
            visible: opacity > 0.01
            opacity: root.kbmOn ? 1 : 0
            transform: Translate { y: root.kbmOn ? 0 : -4; Behavior on y { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
            Behavior on opacity { NumberAnimation { duration: Theme.d(root.kbmOn ? 220 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
            Text {
                id: kbmChipText
                anchors.centerIn: parent
                text: qsTr("clavier-souris actif · ctrl+m touches · ctrl+alt+maj+z libère")
                color: Theme.accentText
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
            }
        }
    }

    // --- keyboard/mouse bindings (Ctrl+M) --------------------------------------------------
    // A compact panel in the bottom-right corner. It sits above the toolbar's place whenever
    // the two would overlap, so the toolbar never hides it, and comes in from the corner:
    // it slides in and fades up, and leaves with a shorter, smaller move.
    Rectangle {
        id: keys
        objectName: "stream-keys"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        readonly property bool shown: root.keysOpen && !root.paused
        readonly property bool overToolbar: root.width - 24 - width < toolbar.x + toolbar.width + 16
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 24
        anchors.bottomMargin: overToolbar ? 24 + toolbar.height + 12 : 24
        width: keysCol.implicitWidth + 40
        height: keysCol.implicitHeight + 40 + 1
        radius: Theme.rLg
        color: Theme.popover
        border.width: 1
        border.color: Theme.stroke2
        visible: opacity > 0.01
        opacity: shown ? 1 : 0
        transformOrigin: Item.BottomRight
        scale: shown || Theme.reduced ? 1 : 0.98
        transform: Translate {
            x: keys.shown || Theme.reduced ? 0 : 12
            y: keys.shown || Theme.reduced ? 0 : 16
            Behavior on x { NumberAnimation { duration: Theme.d(keys.shown ? 260 : 180); easing.type: keys.shown ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
            Behavior on y { NumberAnimation { duration: Theme.d(keys.shown ? 260 : 180); easing.type: keys.shown ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        }
        Behavior on opacity { NumberAnimation { duration: Theme.d(keys.shown ? 220 : 180); easing.type: keys.shown ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        Behavior on scale { NumberAnimation { duration: Theme.d(keys.shown ? 260 : 180); easing.type: keys.shown ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("touches clavier-souris")
        Rectangle { x: parent.radius; y: 1; width: parent.width - 2 * parent.radius; height: 1; color: Theme.lit }
        ColumnLayout {
            id: keysCol
            x: 20
            y: 21
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                VIcon { name: "keyboard"; size: 14; strokeWidth: 1.5; color: Theme.accentText }
                Text { text: qsTr("touches"); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsRow }
                Item { Layout.fillWidth: true; Layout.minimumWidth: 16 }
                Text {
                    text: root.kbmOn ? qsTr("ctrl+m pour fermer") : qsTr("clavier-souris coupé · ctrl+m pour fermer")
                    color: Theme.t3
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                }
                UiButton { kind: "tool"; size: 24; iconSize: 12; iconName: "winclose"; tip: qsTr("fermer"); visible: !root.kbmOn; onClicked: root.keysOpen = false }
            }
            KbmBindings {
                spacing: 24
                // Rebuilt when the mouse mode changes: the camera row names the stick or the gyro.
                bindings: keys.visible ? (veyra.preferences.kbmMouse, veyra.ps5KbmBindings()) : []
            }
        }
    }

    // --- pad paused ----------------------------------------------------------------------
    // Alt-tab out and the PS5 stops getting the pad: a small card in the bottom-right corner,
    // where the key panel sits, says so without covering the game. It comes in from the
    // corner like the key panel and leaves with a shorter move.
    Rectangle {
        id: pause
        objectName: "ps5-paused-card"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        readonly property bool overToolbar: root.width - 24 - width < toolbar.x + toolbar.width + 16
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 24
        anchors.bottomMargin: overToolbar ? 24 + toolbar.height + 12 : 24
        width: Math.min(pauseCol.implicitWidth + 32, root.width - 48)
        height: pauseCol.implicitHeight + 28 + 1
        radius: Theme.rLg
        color: Theme.popover
        border.width: 1
        border.color: Theme.stroke2
        visible: opacity > 0.01
        opacity: root.paused ? 1 : 0
        transformOrigin: Item.BottomRight
        scale: root.paused || Theme.reduced ? 1 : 0.98
        transform: Translate {
            x: root.paused || Theme.reduced ? 0 : 12
            y: root.paused || Theme.reduced ? 0 : 16
            Behavior on x { NumberAnimation { duration: Theme.d(root.paused ? 260 : 180); easing.type: root.paused ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
            Behavior on y { NumberAnimation { duration: Theme.d(root.paused ? 260 : 180); easing.type: root.paused ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        }
        Behavior on opacity { NumberAnimation { duration: Theme.d(root.paused ? 220 : 180); easing.type: root.paused ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        Behavior on scale { NumberAnimation { duration: Theme.d(root.paused ? 260 : 180); easing.type: root.paused ? Easing.BezierSpline : Easing.InOutSine; easing.bezierCurve: Theme.ease } }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: qsTr("manette en pause")
        Rectangle { x: parent.radius; y: 1; width: parent.width - 2 * parent.radius; height: 1; color: Theme.lit }
        ColumnLayout {
            id: pauseCol
            x: 16
            y: 15
            width: parent.width - 32
            spacing: 10
            RowLayout {
                spacing: 8
                VIcon { name: "pausebars"; size: 12; filled: true; color: Theme.t2 }
                Text { text: qsTr("manette en pause"); color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: Theme.fsRow }
            }
            Text {
                Layout.fillWidth: true
                Layout.maximumWidth: 260
                text: qsTr("BooVI est en arrière-plan. clique ou appuie sur ps pour reprendre.")
                color: Theme.t3
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsMeta
                lineHeight: 1.4
                wrapMode: Text.Wrap
            }
            RowLayout {
                spacing: 6
                UiButton {
                    kind: "solid"
                    size: 28
                    fontSize: Theme.fsMeta
                    text: qsTr("reprendre")
                    onClicked: { root.previewPaused = false; Window.window.requestActivate() }
                }
                UiButton {
                    kind: "tool"
                    size: 28
                    text: qsTr("toujours envoyer la manette")
                    onClicked: { if (!root.preview) veyra.ps5ControlSet("background", true); root.previewPaused = false }
                }
            }
        }
    }

    // --- toolbar -------------------------------------------------------------------------
    Rectangle {
        id: toolbar
        objectName: "stream-toolbar"
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        readonly property bool shown: root.preview || shareOpen || ((root.controlsActive || toolHover.hovered) && !root.kbmOn)
        // Partager: the share panel (SharePanel.qml) opens above the toolbar.
        property bool shareOpen: typeof vyTest !== "undefined" && vyTest.sharePanel === true
        readonly property bool shareAvailable: root.preview || (typeof vyGuest !== "undefined" && vyGuest !== null)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: tools.implicitWidth + 8
        height: 48
        radius: Theme.rLg
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        visible: opacity > 0.01
        opacity: shown ? 1 : 0
        transform: Translate { y: toolbar.shown ? 0 : 8; Behavior on y { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(toolbar.shown ? 220 : 150); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
        HoverHandler { id: toolHover }
        DragHandler {
            target: null
            enabled: !root.fullscreen && Window.window && Window.window.visibility !== Window.Maximized
            onActiveChanged: if (active) Window.window.startSystemMove()
        }
        Row {
            id: tools
            anchors.centerIn: parent
            spacing: 4
            UiButton {
                kind: "tool"; iconName: "winclose"; tip: qsTr("quitter le stream")
                onClicked: { if (!root.preview) sources.quit(root.kind); root.requestPage("home") }
            }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 24; color: Theme.line }
            UiSeg {
                anchors.verticalCenter: parent.verticalCenter
                objectName: "stream-preset"
                accessibleName: qsTr("préréglage")
                cellWidth: 104
                cellHeight: 36
                options: presets.options
                current: presets.current
                onPicked: id => presets.apply(id, root.screenHeight)
            }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 24; color: Theme.line }
            UiButton {
                visible: root.kbmAvailable
                kind: "tool"
                text: qsTr("clavier-souris")
                iconName: "keyboard"
                active: root.kbmOn
                onClicked: root.toggleKbm()
            }
            UiButton {
                objectName: "stream-mute"
                kind: "tool"
                iconName: veyra.muted ? "volx" : "vol"
                tip: veyra.muted ? qsTr("remettre le son") : qsTr("couper le son · %1 %").arg(Math.round(veyra.volume * 100))
                active: veyra.muted
                onClicked: veyra.muted = !veyra.muted
            }
            UiButton { kind: "tool"; iconName: "camera"; tip: qsTr("capture d'écran"); onClicked: if (!root.preview) veyra.takeScreenshot() }
            UiButton {
                kind: "tool"
                iconName: "video"
                tip: veyra.recording ? qsTr("arrêter l'enregistrement") : qsTr("enregistrer")
                active: veyra.recording
                inkOverride: veyra.recording ? Theme.err : "transparent"
                onClicked: if (!root.preview) veyra.toggleRecording("toolbar")
            }
            UiButton {
                visible: veyra.replayEnabled
                kind: "tool"
                iconName: "history"
                tip: qsTr("garder le replay")
                onClicked: if (!root.preview) veyra.saveReplay("toolbar")
            }
            UiButton {
                objectName: "stream-share"
                visible: toolbar.shareAvailable
                kind: "tool"
                iconName: "users"
                text: qsTr("partager")
                tip: qsTr("un ami joue avec toi à distance")
                active: toolbar.shareOpen || (typeof vyGuest !== "undefined" && vyGuest !== null && vyGuest.active)
                onClicked: toolbar.shareOpen = !toolbar.shareOpen
            }
            UiButton {
                kind: "tool"
                iconName: "activity"
                tip: root.statsMode === "off" ? qsTr("afficher les stats") : qsTr("masquer les stats")
                active: root.statsMode !== "off"
                onClicked: root.toggleStats()
            }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 24; color: Theme.line }
            UiButton { kind: "tool"; iconName: root.fullscreen ? "shrink" : "expand"; tip: root.fullscreen ? qsTr("quitter le plein écran") : qsTr("plein écran"); onClicked: root.requestFullscreen() }
            UiButton { kind: "tool"; iconName: "tune"; tip: qsTr("réglages"); onClicked: root.requestPage("set") }
        }
    }

    SharePanel {
        objectName: "stream-share-panel"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: toolbar.top
        anchors.bottomMargin: 12
        width: Math.min(420, root.width - 48)
        maxHeight: Math.max(240, toolbar.y - 36)
        preview: root.preview
        open: toolbar.shareOpen && toolbar.shareAvailable
        onCloseRequested: toolbar.shareOpen = false
    }
    JoinRoomChip {
        objectName: "stream-join-room"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        room: root.kind === "join" && sources.join.room ? sources.join.room : ({})
    }
    GuestChip {
        objectName: "stream-guest-chip"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        preview: root.preview && toolbar.shareOpen
        onClicked: toolbar.shareOpen = true
    }
}
