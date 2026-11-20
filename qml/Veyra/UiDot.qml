// A 6px status mark: a filled dot when something is there or working, an empty ring when it
// is not (yet). Neutral by default; give it Theme.warn or Theme.err only for a real problem.
// It never pulses or glows: the words next to it carry the state.
import QtQuick

Rectangle {
    id: dot
    property bool hollow: false
    property color tone: Theme.ok
    property real size: 6
    implicitWidth: size
    implicitHeight: size
    radius: size / 2
    color: hollow ? "transparent" : tone
    border.width: hollow ? 1 : 0
    border.color: Theme.t3
    Behavior on color { ColorAnimation { duration: Theme.d(Theme.durState) } }
}
