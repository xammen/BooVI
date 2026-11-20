// Staged entrance for one block of a page: opacity 0 -> 1 and a 6px rise over 420 ms,
// cubic-bezier(0.22, 1, 0.36, 1), delayed by `order`. It plays when its page enters (VPage.enter)
// and when `replay()` is called; it never runs on a state change, and reduced motion shows
// the block at rest. dy < 0 drops it in from above (the stream overlay's top chips).
import QtQuick

Item {
    id: e
    required property Item target
    property int order: 0
    property real dy: 6
    property int stagger: 60
    property int duration: Theme.durEnter
    property QtObject page: null
    visible: false
    Translate { id: shift }
    Component.onCompleted: target.transform = [shift]
    Connections {
        target: e.page
        ignoreUnknownSignals: true
        function onEnter() { e.replay() }
    }
    function replay() {
        anim.stop()
        if (Theme.reduced) { e.target.opacity = 1; shift.y = 0; return }
        e.target.opacity = 0
        shift.y = e.dy
        anim.start()
    }
    SequentialAnimation {
        id: anim
        PauseAnimation { duration: 40 + e.order * e.stagger }
        ParallelAnimation {
            NumberAnimation { target: e.target; property: "opacity"; to: 1; duration: e.duration; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
            NumberAnimation { target: shift; property: "y"; to: 0; duration: e.duration; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease }
        }
    }
}
