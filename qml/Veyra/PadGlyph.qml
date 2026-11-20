// One DualSense control drawn the PlayStation way, for the bindings card: the four face
// shapes in their classic tints (muted), shoulder and trigger tags, the sticks as a ring
// (an arrow inside for a direction, the letter for the camera stick, L3/R3 for a press),
// the d-pad with its pressed arm filled, options, create, the touchpad and the gyro.
// `control` takes the ids of veyra.ps5KbmBindings(): cross, l2, ls-up, up, touchpad...
import QtQuick

Item {
    id: glyph
    property string control
    property real size: 20
    implicitWidth: 24
    implicitHeight: size

    readonly property string kind: {
        const c = control
        if (c === "cross" || c === "circle" || c === "square" || c === "triangle") return "face"
        if (c === "l1" || c === "r1") return "shoulder"
        if (c === "l2" || c === "r2") return "trigger"
        if (c.indexOf("ls-") === 0) return "move"
        if (c === "rs" || c === "l3" || c === "r3" || c === "ps") return "stick"
        if (c === "up" || c === "down" || c === "left" || c === "right") return "dpad"
        return "icon"
    }
    // Classic face colours, pulled toward the text grey so they sit quietly on the card.
    readonly property var tints: ({ triangle: "#6CCDB3", circle: "#E57F8B", cross: "#86A3E6", square: "#CC90C6" })

    VIcon {
        visible: glyph.kind === "face"
        anchors.centerIn: parent
        name: "ps-" + glyph.control
        size: glyph.size
        strokeWidth: 2.2
        color: glyph.tints[glyph.control] || Theme.t1
    }

    // L1/R1 a flat bumper, L2/R2 a taller trigger with a round top.
    Rectangle {
        visible: glyph.kind === "shoulder" || glyph.kind === "trigger"
        anchors.centerIn: parent
        width: 24
        height: glyph.kind === "trigger" ? 18 : 15
        topLeftRadius: glyph.kind === "trigger" ? 9 : 5
        topRightRadius: topLeftRadius
        bottomLeftRadius: Theme.rSm
        bottomRightRadius: Theme.rSm
        color: Theme.card3
        border.width: 1
        border.color: Theme.stroke2
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            text: glyph.control.toUpperCase()
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: 9
        }
    }

    // Sticks: the ring, then what is inside it.
    Rectangle {
        visible: glyph.kind === "stick" && glyph.control !== "rs"
        anchors.centerIn: parent
        width: glyph.size * 0.79
        height: width
        radius: width / 2
        color: Theme.card3
    }
    VIcon {
        visible: glyph.kind === "move" || glyph.kind === "stick"
        anchors.centerIn: parent
        name: "ps-stick"
        size: glyph.size
        strokeWidth: 1.6
        color: Theme.t2
    }
    VIcon {
        visible: glyph.kind === "move"
        anchors.centerIn: parent
        name: "ps-chevron-" + glyph.control.slice(3)
        size: glyph.size
        strokeWidth: 2.2
        color: Theme.t1
    }
    Text {
        visible: glyph.kind === "stick"
        anchors.centerIn: parent
        text: glyph.control.toUpperCase()
        color: Theme.t1
        font.family: Theme.fontMono
        font.pixelSize: glyph.control.length > 1 ? 8 : 9
    }

    // D-pad: every arm drawn, the bound one filled.
    VIcon {
        visible: glyph.kind === "dpad"
        anchors.centerIn: parent
        name: "ps-dpad"
        size: glyph.size
        strokeWidth: 1.4
        color: Theme.t3
    }
    VIcon {
        visible: glyph.kind === "dpad"
        anchors.centerIn: parent
        name: "ps-dpad-" + glyph.control
        size: glyph.size
        filled: true
        color: Theme.t1
    }

    VIcon {
        visible: glyph.kind === "icon"
        anchors.centerIn: parent
        name: glyph.control === "gyro" ? "rotatecw" : "ps-" + glyph.control
        size: glyph.size
        strokeWidth: 1.8
        color: Theme.t1
    }
}
