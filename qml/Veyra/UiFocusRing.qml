// Keyboard and controller focus: a 2px violet ring 2px outside its parent, its radius the
// parent's plus the gap so the two curves stay concentric. It shows and moves at once: focus
// travels with every d-pad press, so it never waits on an animation.
import QtQuick

Rectangle {
    property real innerRadius: 0
    property real gap: 2
    anchors.fill: parent
    anchors.margins: -gap - border.width
    radius: innerRadius > 0 ? innerRadius + gap + border.width : 0
    color: "transparent"
    border.width: 2
    border.color: Theme.focusRing
}
