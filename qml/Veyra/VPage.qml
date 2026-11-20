// A page of the shell. Main.qml says which page is current (`page`); each page eases its
// own presence `t` toward 1 when it becomes current and toward 0 when it stops being.
// The incoming page rises 6px into place over 320 ms after a 60 ms beat; the outgoing one only
// fades, over 160 ms on an accelerating curve, so the two barely overlap. A switch in the middle
// of another restarts from where each page is, so a quick second click never jumps.
// Reduced motion snaps both ends. enter() is emitted each time the page becomes current.
import QtQuick

Item {
    id: page
    property string pageId
    signal enter()
    // The item the page sits in (Main.qml's page stack). A page that is neither current nor
    // fading out leaves the scene: its live bindings kept changing while hidden and Qt
    // redrew the whole window for each change (~11 times a second during capture), which
    // cost GPU time and kept OBS game capture on the UI instead of the video.
    property Item home: null
    readonly property var shell: home ? home.Window.window : null
    readonly property bool current: shell !== null && shell.page === pageId
    // Pages under the shell's top bar start below it; the stream page goes edge to edge.
    property bool underBar: true
    property real t: 0
    readonly property bool moving: motion.running
    parent: current || t > 0 ? home : null
    visible: current || t > 0
    enabled: current
    y: underBar && shell ? shell.barHeight : 0
    width: home ? home.width : 0
    height: home ? home.height - y : 0
    opacity: t
    transform: Translate { id: shift }

    SequentialAnimation {
        id: motion
        PauseAnimation { id: beat }
        ParallelAnimation {
            NumberAnimation { id: fade; target: page; property: "t"; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
            NumberAnimation { id: slide; target: shift; property: "y"; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
        }
    }
    function settle() {
        motion.stop()
        t = current ? 1 : 0
        shift.y = 0
    }
    onCurrentChanged: {
        if (current) enter()
        if (Theme.reduced) { settle(); return }
        const fresh = current && t === 0
        motion.stop()
        if (fresh) shift.y = 6
        beat.duration = fresh ? 60 : 0
        fade.to = current ? 1 : 0
        slide.to = 0
        fade.duration = slide.duration = current ? 320 : 160
        fade.easing.bezierCurve = current ? Theme.ease : Theme.easeExit
        motion.start()
    }
    Component.onCompleted: settle()

    // Title-bar behaviour on the page's top strip (field request 2026-10-01: only the
    // dock capsule moved the window). Below the page's own items, so buttons and fields
    // there keep their clicks; empty space drags, a double click maximizes. Pages whose
    // top is the picture set a smaller strip (node) or none (min).
    property real dragStripHeight: 56
    Item {
        objectName: "page-drag-strip"
        z: -1
        x: 0; y: 0
        width: page.width
        height: page.dragStripHeight
        visible: height > 0
        DragHandler {
            target: null
            enabled: page.shell !== null && !page.shell.fullscreen && !page.shell.maximized
            onActiveChanged: if (active) page.shell.startSystemMove()
        }
        TapHandler {
            enabled: page.shell !== null && !page.shell.fullscreen
            onDoubleTapped: page.shell.toggleMaximized()
        }
    }
}
