// The application shell of the streaming fork (2026-11 mockups).
//
// Four pages: home (the sources you set up, one click to play), consoles (each source's
// three-step setup), set (réglages) and min (the stream, edge to edge, with its floating
// overlay). A frameless window with an 8px radius; on the stream page the window snaps to
// the stream's aspect so the picture has no bars.
//
// The video is not in this scene graph. It is the native D3D12 child window that
// apps/veyra-qml/main.cpp places over the item named "videoHost"; anything floating over
// it is a videoCover, cut out of that window's region.
//
// --page stream-preview[-paused][-kbm][-keys][-nopad] shows the stream page and its overlay without a
// session (sample numbers, no engine call), so the layout can be checked offline.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: root
    width: 1280
    height: 800
    minimumWidth: cinema ? 360 : 960
    minimumHeight: 260
    visible: true
    color: typeof vySoftwareUi !== "undefined" && vySoftwareUi ? (cinema ? Theme.videoBlack : Theme.bgOuter) : "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint
           | (veyra.passthroughActive ? Qt.WindowTransparentForInput : 0)
           | (veyra.gameInputActive ? Qt.WindowDoesNotAcceptFocus : 0)
    title: "BooVI"

    property string page: "home"
    readonly property var test: typeof vyTest !== "undefined" ? vyTest : ({})
    readonly property real testAspect: test.aspect !== undefined ? test.aspect : 0
    property bool preview: false
    readonly property bool cinema: page === "min"
    readonly property bool maximized: visibility === Window.Maximized
    readonly property bool screenSized: fullTarget || fullRestorePending || visibility === Window.FullScreen || visibility === Window.Maximized
    readonly property var pageIds: ["home", "consoles", "set", "min"]

    // Pages the fork no longer has (pro, node, exp) land on home.
    function goPage(p) { page = pageIds.indexOf(p) >= 0 ? p : "home" }
    function openSetup(source, step) { connectPage.show(source || "", step); goPage("consoles") }
    function toggleMaximized() {
        if (fullscreen) { toggleFullscreen(); return }
        visibility = maximized ? Window.Windowed : Window.Maximized
    }
    Binding { target: Theme; property: "reduced"; value: veyra.reducedMotion || root.test.reducedMotion === true }
    Binding { target: Theme; property: "accentName"; value: veyra.preferences.accent || "orange" }
    Binding { target: Theme; property: "paletteName"; value: veyra.preferences.palette || "sage" }

    // --- cinema geometry ----------------------------------------------------------------
    property real filmAspect: 16 / 9
    readonly property real reportedAspect: veyra.previewAspect
    onReportedAspectChanged: if (reportedAspect < 0.2) fitToFilm(16 / 9)
    property string lastPage: "home"
    property real heightBeforeCinema: 0
    readonly property rect screenArea: veyra.screenAvailableAt(root.x + root.width / 2, root.y + 20)
    readonly property real maxPictureHeight: Math.max(200, root.screenArea.height - 16)
    readonly property real pictureHeight: Math.min(Math.round(width / filmAspect), maxPictureHeight)
    property real widthBeforeNarrow: 0

    function fitToFilm(aspect) {
        if (aspect > 0.2 && aspect < 5.0) filmAspect = aspect
        if (page !== "min" || screenSized) return
        let w = widthBeforeNarrow > 0 ? widthBeforeNarrow : width
        if (Math.round(w / filmAspect) > maxPictureHeight)
            w = Math.max(minimumWidth, Math.round(maxPictureHeight * filmAspect))
        if (Math.abs(w - width) >= 1) {
            if (widthBeforeNarrow <= 0) widthBeforeNarrow = width
            x += Math.round((width - w) / 2)
            width = w
        }
        if (widthBeforeNarrow > 0 && w >= widthBeforeNarrow) widthBeforeNarrow = 0
        height = Math.round(pictureHeight)
        const a = root.screenArea
        if (y + pictureHeight + 8 > a.y + a.height) y = Math.max(a.y, a.y + a.height - pictureHeight - 8)
    }
    Behavior on height {
        enabled: root.cinema && !root.screenSized
        NumberAnimation { duration: Theme.d(500); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
    }
    onWidthChanged: {
        if (cinema && !screenSized) height = Math.round(pictureHeight)
        videoHost.syncRect()
    }
    onHeightChanged: videoHost.syncRect()
    onMaximizedChanged: {
        if (visibility === Window.Windowed && cinema && !fullTarget && !fullRestorePending) height = Math.round(pictureHeight)
        videoHost.syncRect()
    }
    onPageChanged: {
        if (veyra.currentPage !== page) veyra.currentPage = page
        const wasCinema = lastPage === "min"
        lastPage = page
        if (!screenSized && wasCinema && page !== "min" && widthBeforeNarrow > 0) {
            x -= Math.round((widthBeforeNarrow - width) / 2)
            width = widthBeforeNarrow
            widthBeforeNarrow = 0
        }
        if (!screenSized && page !== "min" && width < 960) { x -= Math.round((960 - width) / 2); width = 960 }
        if (screenSized) { /* the screen is the window */ }
        else if (page === "min") { if (!wasCinema) heightBeforeCinema = height; fitToFilm(filmAspect) }
        else if (wasCinema && heightBeforeCinema >= 400) height = heightBeforeCinema
        else if (height < 600) height = 800
        videoHost.syncRect()
    }
    readonly property real barHeight: topBar.implicitHeight
    readonly property bool navigating: homePage.moving || connectPage.moving || settingsPage.moving || minPage.moving

    // The window's own background stays under the pages until the stream page has fully
    // come in, so nothing behind the window shows through a cross-fade.
    Rectangle {
        anchors.fill: parent
        visible: minPage.t < 1
        radius: root.screenSized ? 0 : Theme.rWindow
        color: Theme.bgOuter
        border.width: root.screenSized ? 0 : 1
        border.color: Theme.line
    }

    // The one host item for the native video window; main.cpp finds it by name.
    Item {
        id: videoHost
        objectName: "videoHost"
        visible: false
        function syncRect() {
            const r = root.videoRect()
            x = r.x; y = r.y; width = r.width; height = r.height
        }
    }
    function videoRect() {
        if (fullscreen && veyra.gameMode && veyra.gameVideoRect.width > 0 && veyra.gameVideoRect.height > 0)
            return veyra.gameVideoRect
        if (page !== "min") return { x: 0, y: 0, width: 0, height: 0 }
        if (fullscreen || maximized) return { x: 0, y: 0, width: width, height: height }
        // The window can end up shorter than the picture (Windows keeps it on its monitor).
        return { x: 0, y: 0, width: width, height: Math.min(Math.round(pictureHeight), height) }
    }

    Item {
        id: pages
        anchors.fill: parent
        HomePage {
            id: homePage
            pageId: "home"
            home: pages
            onRequestPage: p => root.goPage(p)
            onRequestSetup: s => root.openSetup(s)
        }
        ConnectPage {
            id: connectPage
            pageId: "consoles"
            home: pages
            onRequestPage: p => root.goPage(p)
            onRequestAdvancedCapture: dialogs.open("capture")
        }
        SettingsPage { id: settingsPage; pageId: "set"; home: pages; onRequestPage: p => root.goPage(p) }
        MinimalPage {
            id: minPage
            pageId: "min"
            underBar: false
            home: pages
            preview: root.preview
            onRequestAspect: aspect => { if (root.testAspect <= 0) root.fitToFilm(aspect) }
        }
    }

    // One bar for accueil, consoles and réglages: it stays put while the pages under it
    // switch, and fades with the stream page.
    UiTopBar {
        id: topBar
        width: parent.width
        current: root.page
        opacity: 1 - minPage.t
        visible: opacity > 0
        onNavigate: p => {
            if (p === root.page) return
            if (p === "consoles") root.openSetup("")
            else root.goPage(p)
        }
    }

    StreamOverlay {
        id: overlay
        x: videoHost.x
        y: videoHost.y
        width: Math.max(videoHost.width, 1)
        height: Math.max(videoHost.height, 1)
        z: 90
        enabled: minPage.current
        opacity: minPage.t
        preview: root.preview
        fullscreen: root.fullscreen
        controlsActive: root.controlsActive
        onRequestPage: p => {
            if (root.fullscreen && p !== "min") root.toggleFullscreen()
            if (root.preview && p === "home") root.preview = false
            root.goPage(p)
        }
        onRequestFullscreen: root.toggleFullscreen()
    }

    // The full capture dialog ("réglages avancés" of a capture card) and its scrim.
    DialogHost {
        id: dialogs
        anchors.fill: parent
        onStartCapture: root.goPage("min")
    }

    // Frameless window: the system resizes from 6px borders and 12px corners.
    Repeater {
        model: [
            { e: Qt.LeftEdge, c: Qt.SizeHorCursor }, { e: Qt.RightEdge, c: Qt.SizeHorCursor },
            { e: Qt.TopEdge, c: Qt.SizeVerCursor }, { e: Qt.BottomEdge, c: Qt.SizeVerCursor },
            { e: Qt.LeftEdge | Qt.TopEdge, c: Qt.SizeFDiagCursor }, { e: Qt.RightEdge | Qt.BottomEdge, c: Qt.SizeFDiagCursor },
            { e: Qt.RightEdge | Qt.TopEdge, c: Qt.SizeBDiagCursor }, { e: Qt.LeftEdge | Qt.BottomEdge, c: Qt.SizeBDiagCursor }
        ]
        delegate: MouseArea {
            required property var modelData
            readonly property bool l: (modelData.e & Qt.LeftEdge) !== 0
            readonly property bool r: (modelData.e & Qt.RightEdge) !== 0
            readonly property bool t: (modelData.e & Qt.TopEdge) !== 0
            readonly property bool b: (modelData.e & Qt.BottomEdge) !== 0
            readonly property bool corner: (l || r) && (t || b)
            z: 130
            enabled: !root.fullscreen && !root.maximized
            visible: enabled
            width: corner ? 12 : (l || r) ? 6 : root.width - 24
            height: corner ? 12 : (t || b) ? 6 : root.height - 24
            x: l ? 0 : r ? root.width - width : 12
            y: t ? 0 : b ? root.height - height : 12
            cursorShape: modelData.c
            onPressed: root.startSystemResize(modelData.e)
        }
    }

    // --- fullscreen ------------------------------------------------------------------------
    readonly property bool fullscreen: visibility === Window.FullScreen
    property bool fullTarget: false
    property int fullRestoreVisibility: Window.Windowed
    property bool fullRestorePending: false
    onFullscreenChanged: {
        veyra.setPresentationFullscreen(fullscreen)
        fullTarget = fullscreen
        if (!fullscreen && cinema && visibility === Window.Windowed && !fullRestorePending) height = Math.round(pictureHeight)
        pointerActivity()
        videoHost.syncRect()
    }
    function toggleFullscreen() {
        if (!fullscreen && veyra.fullscreenMemorySafe === false) {
            toast.show(qsTr("le plein écran est coupé pour cette session : la mémoire vidéo ne cessait de grimper."), true)
            return
        }
        if (!fullscreen) {
            fullRestoreVisibility = maximized ? Window.Maximized : Window.Windowed
            fullTarget = true
            visibility = Window.FullScreen
        } else {
            fullRestorePending = fullRestoreVisibility === Window.Maximized
            fullTarget = false
            visibility = fullRestoreVisibility
            Qt.callLater(function() { root.fullRestorePending = false })
        }
        veyra.logUi("ui-fullscreen", "enabled=" + fullscreen)
    }

    // The stream's toolbar shows while the pointer moves and hides 1.6 s after it stops,
    // with the cursor in fullscreen. The native video window passes pointer moves through,
    // so this window-wide handler sees them over the picture too.
    property bool controlsActive: true
    function pointerActivity() {
        if (!controlsActive) controlsActive = true
        controlsIdle.restart()
    }
    Timer {
        id: controlsIdle
        interval: 1600
        onTriggered: { if (overlay.toolbarHovered) restart(); else root.controlsActive = false }
    }
    HoverHandler {
        cursorShape: root.fullscreen && !root.controlsActive && root.page === "min" ? Qt.BlankCursor : Qt.ArrowCursor
        property point last: Qt.point(-1, -1)
        onPointChanged: {
            const p = point.scenePosition
            if (Math.abs(p.x - last.x) < 1 && Math.abs(p.y - last.y) < 1) return
            last = p
            root.pointerActivity()
        }
    }
    function pictureContains(p) {
        return p.x >= videoHost.x && p.y >= videoHost.y && p.x <= videoHost.x + videoHost.width && p.y <= videoHost.y + videoHost.height
    }
    function overVideoCover(scenePoint) {
        const visit = item => {
            if (!item || !item.visible || item.opacity <= 0) return false
            if (item.objectName === "videoCover" || item.videoCover === true) {
                const q = item.mapFromItem(null, scenePoint.x, scenePoint.y)
                if (q.x >= 0 && q.y >= 0 && q.x <= item.width && q.y <= item.height) return true
            }
            for (let i = 0; i < item.children.length; ++i) if (visit(item.children[i])) return true
            return false
        }
        return visit(root.contentItem.parent ? root.contentItem.parent : root.contentItem)
    }
    // A double click on the picture toggles fullscreen.
    TapHandler {
        enabled: !veyra.gameInputActive && root.page === "min"
        property bool pressOnCover: false
        property bool pressOnPicture: false
        onPressedChanged: if (pressed) {
            pressOnCover = root.overVideoCover(point.scenePosition)
            pressOnPicture = root.pictureContains(point.scenePosition)
        }
        onDoubleTapped: if (!pressOnCover && pressOnPicture && dialogs.dialog === "") root.toggleFullscreen()
    }
    // A click on the picture of a PC stream gives the keyboard and mouse back to the host
    // after a release (Ctrl+Alt+Shift+Z). While captured the clicks never get here.
    TapHandler {
        enabled: veyra.moonlight && veyra.moonlight.state.streaming === true && !veyra.moonlightCaptured && dialogs.dialog === ""
        property bool pressOnCover: false
        property bool pressOnPicture: false
        onPressedChanged: if (pressed) {
            pressOnCover = root.overVideoCover(point.scenePosition)
            pressOnPicture = root.pictureContains(point.scenePosition)
        }
        onTapped: if (!pressOnCover && pressOnPicture && root.page === "min") veyra.moonlightCapture(true)
    }

    Shortcut { sequence: "Ctrl+Alt+Shift+S"; enabled: root.page === "min"; onActivated: overlay.toggleStats() }
    Shortcut { sequence: veyra.shortcuts.fullscreen; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: veyra.shortcuts.screenshot; onActivated: veyra.takeScreenshot() }
    Shortcut { sequence: veyra.shortcuts.record; autoRepeat: false; onActivated: veyra.toggleRecording("shortcut") }
    Shortcut { sequence: veyra.shortcuts.replay; autoRepeat: false; onActivated: veyra.saveReplay("shortcut") }
    Shortcut { sequences: ["F11", "Alt+Return", "Alt+Enter"]; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "Esc"; enabled: root.fullscreen; onActivated: root.toggleFullscreen() }

    // --- toasts and notices ---------------------------------------------------------------
    UpdateBanner {
        id: updateBanner
        z: 35
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 24
        anchors.bottomMargin: 24
        held: root.page === "min"
        onOpenRequested: veyra.updates.openRelease()
    }
    Connections {
        target: veyra.updates
        function onUpdateAvailable(version) { updateBanner.show(version) }
    }
    // Test switch --preset <performance|balanced|fluid>: the toolbar's preset, applied once the
    // first source shows a picture (two-instance runs set host and guest apart).
    StreamPresets { id: testPresets }
    property bool testPresetApplied: false
    Connections {
        target: veyra
        enabled: typeof vyTest !== "undefined" && (vyTest.preset || "").length > 0 && !root.testPresetApplied
        function onSnapshotChanged() {
            if (!veyra.hasSource || veyra.sourceHeight <= 0) return
            root.testPresetApplied = true
            if ((vyTest.fgBackend || "").length > 0) veyra.fgBackendName = vyTest.fgBackend
            if ((vyTest.fgMotion || "").length > 0) veyra.fgMotionSource = vyTest.fgMotion === "zero" ? 0 : 1
            testPresets.apply(vyTest.preset, Math.round(Screen.height * Screen.devicePixelRatio))
        }
    }
    Rectangle {
        id: toast
        z: 120
        property string message: ""
        property bool isError: false
        property bool on: false
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 16
        width: Math.min(toastText.implicitWidth + 32, root.width - 48)
        height: toastText.implicitHeight + 20
        radius: Theme.rLg
        color: Theme.popover
        border.width: 1
        border.color: toast.isError ? Qt.rgba(1, 0.365, 0.365, 0.45) : Theme.stroke2
        opacity: on ? 1 : 0
        visible: opacity > 0.01
        transform: Translate { y: toast.on ? 0 : -8; Behavior on y { NumberAnimation { duration: Theme.d(260); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(toast.on ? 220 : 150) } }
        function show(m, bad) { toast.message = m; toast.isError = bad; toast.on = true; toastTimer.restart() }
        Timer { id: toastTimer; interval: 3200; onTriggered: toast.on = false }
        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 80)
            text: toast.message
            color: toast.isError ? "#FFC9C9" : Theme.t1
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsText
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // RivaTuner, NVIDIA's overlay or GamePP started after the app: offer the restart that
    // keeps their overlay on the picture only.
    VConfirm {
        id: overlayRestart
        objectName: "overlay-restart-confirm"
        property string kind: "rtss"
        title: kind === "rtss" ? qsTr("rivatuner détecté") : kind === "nvidia" ? qsTr("overlay nvidia détecté") : qsTr("gamepp détecté")
        text: qsTr("pour que son affichage reste sur l'image du jeu et pas sur l'interface, BooVI doit redémarrer. le stream en cours s'arrête.")
        acceptText: qsTr("redémarrer")
        rejectText: qsTr("plus tard")
        onAccepted: veyra.restartForOverlayCompatibility()
    }

    // Invités (fork): a friend's link knocks; the host answers here (or from the tray).
    Rectangle {
        id: accessAsk
        objectName: "access-ask"
        z: 125
        readonly property var acc: typeof vyAccess !== "undefined" && vyAccess ? vyAccess : null
        readonly property var p: acc ? acc.pending : ({})
        readonly property bool on: p.name !== undefined
        readonly property bool videoCover: true
        property real coverRadius: Theme.rLg
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: Math.min(360, root.width - 48)
        height: askCol.implicitHeight + 32
        radius: Theme.rLg
        color: Theme.popover
        border.width: 1
        border.color: Theme.stroke2
        opacity: on ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.d(200) } }
        ColumnLayout {
            id: askCol
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: accessAsk.p.busy ? qsTr("%1 voudrait te rejoindre").arg(accessAsk.p.name || "") : qsTr("%1 veut jouer sur ta ps5").arg(accessAsk.p.name || "")
                color: Theme.t1
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsRow
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: (accessAsk.p.busy ? (accessAsk.p.mode === "together" ? qsTr("à deux, sur ta partie en cours") : qsTr("il regarde ta partie"))
                                        : qsTr("la console se réveille et il joue seul"))
                      + " · " + qsTr("refus auto dans %1 s").arg(accessAsk.p.left || 0)
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
                wrapMode: Text.Wrap
            }
            RowLayout {
                spacing: 8
                UiButton { text: qsTr("accepter"); kind: "solid"; size: 32; focusPolicy: Qt.NoFocus; onClicked: accessAsk.acc.approve() }
                UiButton { text: qsTr("refuser"); size: 32; onClicked: accessAsk.acc.refuse() }
            }
        }
    }
    Connections {
        target: typeof vyAccess !== "undefined" && vyAccess ? vyAccess : null
        ignoreUnknownSignals: true
        function onNotice(text, warning) { toast.show(text, warning) }
    }

    // Partager (fork): the guest share's own notices (joined, left, link copied, tailscale).
    Connections {
        target: typeof vyGuest !== "undefined" && vyGuest ? vyGuest : null
        ignoreUnknownSignals: true
        function onNotice(text, warning) { toast.show(text, warning) }
    }
    Connections {
        target: veyra
        function onNotice(text, isError) { toast.show(text, isError) }
        function onOverlayRestartSuggested(kind) { overlayRestart.kind = kind; overlayRestart.open() }
        // Any stream that starts (play button, resume, --play-last) shows the stream page.
        function onMediaOpenRequested() { if (!root.preview) root.goPage("min") }
        function onNavigate(p) {
            const setup = ({ ps5: "ps5", moonlight: "pc", xbox: "xbox" })[p]
            if (setup) { root.openSetup(setup); return }
            if (p === "capture") { dialogs.open("capture"); return }
            root.goPage(p)
        }
        function onSnapshotChanged() {
            if (root.fullscreen && veyra.fullscreenMemorySafe === false) root.toggleFullscreen()
            if (root.testAspect > 0) return
            if (veyra.previewAspect > 0.2 && Math.abs(veyra.previewAspect - root.filmAspect) > 0.02)
                root.fitToFilm(veyra.previewAspect)
        }
        function onGameModeChanged() { videoHost.syncRect() }
    }
    // A stream that ends on its own (the console went to rest, the host quit) returns home.
    readonly property bool streaming: veyra.hasSource || veyra.liveOpeningText.length > 0
    onStreamingChanged: if (!streaming && page === "min" && !preview) goPage("home")

    function applyStartupSize() {
        const choice = veyra.preferences.windowSize || "1280x800"
        const text = choice === "last" ? (veyra.preferences.lastWindow || "1280x800") : choice
        const wh = text.split("x").map(Number)
        if (wh.length !== 2 || !(wh[0] > 0) || !(wh[1] > 0)) return
        const a = veyra.launchScreenAvailable()
        root.width = Math.max(root.minimumWidth, Math.min(wh[0], a.width - 40))
        root.height = Math.max(root.minimumHeight, Math.min(wh[1], a.height - 40))
        root.x = a.x + Math.round((a.width - root.width) / 2)
        root.y = a.y + Math.round((a.height - root.height) / 2)
    }
    onClosing: if (!root.fullscreen && !root.maximized && !root.cinema) veyra.rememberWindowSize(root.width, root.height)

    // --reconnect <quitMs>,<playMs> (test): leave the PS5 stream the way the toolbar's quit
    // does, then play it again from the home page, muted, to see what shows between sessions.
    Timer {
        id: reconnectProbe
        readonly property var steps: root.test.reconnect !== undefined ? String(root.test.reconnect).split(",").map(Number) : []
        property bool quitDone: false
        running: steps.length === 2
        interval: steps[0] || 1
        Component.onCompleted: if (running) veyra.muted = true
        onTriggered: {
            if (!quitDone) {
                veyra.logUi("ui-reconnect", "quit")
                veyra.ps5Cancel()
                root.goPage("home")
                quitDone = true
                interval = steps[1]
                start()
            } else {
                veyra.logUi("ui-reconnect", "play")
                veyra.muted = true
                homePage.play("ps5")
            }
        }
    }

    Component.onCompleted: {
        veyra.updates.startAutomaticCheck()
        applyStartupSize()
        // The node view is gone: the chain stays a list.
        if (veyra.nodeMode !== 0) veyra.nodeMode = 0
        const initial = veyra.initialPage
        if (initial.indexOf("stream-preview") === 0) {
            root.preview = true
            overlay.previewPaused = initial.indexOf("paused") > 0
            overlay.previewKbm = initial.indexOf("kbm") > 0
            overlay.keysOpen = initial.indexOf("keys") > 0
            overlay.previewNoPad = initial.indexOf("nopad") > 0
            root.page = "min"
        } else if (initial.indexOf("consoles") === 0) {
            const parts = initial.split("-")
            root.openSetup(parts[1] || "ps5", parts.length > 2 ? Number(parts[2]) : -1)
        } else if (initial.indexOf("set") === 0 && initial.length > 4) {
            settingsPage.section = initial.slice(4)
            root.page = "set"
        } else if (initial.length > 0) {
            root.goPage(initial)
        }
        if (root.test.dialog !== undefined) {
            const d = String(root.test.dialog)
            if (d === "capture-advanced") dialogs.open("capture")
            else root.openSetup(({ moonlight: "pc" })[d.split("-")[0]] || d.split("-")[0], d.indexOf("-") > 0 ? Number(d.split("-")[1]) : -1)
        }
        if (root.testAspect > 0) root.fitToFilm(root.testAspect)
        videoHost.syncRect()
        if (root.page === "home") homePage.intro()
        controlsIdle.restart()
    }
}
