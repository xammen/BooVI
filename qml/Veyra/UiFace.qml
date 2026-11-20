// The fork's face mark. It blinks every 3 to 8 s (a double blink now and then) and stays
// open under reduced motion.
import QtQuick

Text {
    id: face
    property bool closed: false
    text: closed ? "༼ つ - - ༽つ" : "༼ つ ╹ ╹ ༽つ"
    color: Theme.t1
    font.family: "Segoe UI"
    font.pixelSize: 13
    Accessible.ignored: true
    property int blinksLeft: 0
    Timer {
        id: wait
        running: face.visible && !Theme.reduced
        interval: 3000 + Math.random() * 5000
        onTriggered: {
            face.blinksLeft = Math.random() < 0.3 ? 2 : 1
            face.closed = true
            shut.restart()
        }
    }
    Timer {
        id: shut
        interval: 120
        onTriggered: {
            face.closed = false
            if (--face.blinksLeft > 0) again.restart()
            else { wait.interval = 3000 + Math.random() * 5000; wait.restart() }
        }
    }
    Timer { id: again; interval: 250; onTriggered: { face.closed = true; shut.restart() } }
}
