// What each key and mouse input does on the DualSense, grouped (déplacement, caméra, actions,
// croix directionnelle, gâchettes, système). The list comes from the bridge, which reads
// KbmMapper's own bindings, so it shows what the mapper really sends. One row per control:
// the control drawn as on the pad (PadGlyph), then every input that presses it as a keycap.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root
    // [{group, control, key | mouse}] from veyra.ps5KbmBindings().
    property var bindings: []
    // Groups per column, top to bottom.
    property var layout: [["move", "camera", "system"], ["actions"], ["triggers", "dpad"]]
    spacing: 32

    readonly property var groupNames: ({
        move: qsTr("déplacement"), camera: qsTr("caméra"), actions: qsTr("actions"),
        dpad: qsTr("croix directionnelle"), triggers: qsTr("gâchettes"), system: qsTr("système")
    })
    // Spoken and hover names of the controls, in row order.
    readonly property var controlNames: ({
        "ls-up": qsTr("stick gauche ↑"), "ls-down": qsTr("stick gauche ↓"), "ls-left": qsTr("stick gauche ←"), "ls-right": qsTr("stick gauche →"),
        rs: qsTr("stick droit"), gyro: qsTr("gyroscope"),
        cross: qsTr("croix"), circle: qsTr("rond"), square: qsTr("carré"), triangle: qsTr("triangle"),
        l3: "l3", r3: "r3",
        up: qsTr("haut"), down: qsTr("bas"), left: qsTr("gauche"), right: qsTr("droite"),
        l1: "l1", r1: "r1", l2: "l2", r2: "r2",
        options: qsTr("options"), create: qsTr("créer"), touchpad: qsTr("pavé tactile"), ps: "ps"
    })
    readonly property var mouseNames: ({
        move: qsTr("souris"), left: qsTr("clic gauche"), right: qsTr("clic droit"), middle: qsTr("clic molette"),
        x1: qsTr("bouton 4"), x2: qsTr("bouton 5"), "wheel-up": qsTr("molette ↑"), "wheel-down": qsTr("molette ↓")
    })
    // {group: [{control, inputs: [label]}]}, rows in controlNames order.
    readonly property var groups: {
        const byGroup = {}
        for (const b of bindings) {
            const rows = byGroup[b.group] || (byGroup[b.group] = [])
            let row = rows.find(r => r.control === b.control)
            if (!row) rows.push(row = { control: b.control, inputs: [] })
            row.inputs.push(b.mouse ? (mouseNames[b.mouse] || b.mouse) : String(b.key).toLowerCase())
        }
        const rank = Object.keys(controlNames)
        for (const g in byGroup) byGroup[g].sort((a, b) => rank.indexOf(a.control) - rank.indexOf(b.control))
        return byGroup
    }

    Repeater {
        model: root.layout
        delegate: ColumnLayout {
            required property var modelData
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 16
            Repeater {
                model: modelData.filter(g => root.groups[g])
                delegate: ColumnLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        Layout.bottomMargin: 2
                        text: root.groupNames[modelData] || modelData
                        color: Theme.t3
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsLabel
                        font.letterSpacing: 0.8
                    }
                    Repeater {
                        model: root.groups[modelData]
                        delegate: Row {
                            id: row
                            required property var modelData
                            spacing: 8
                            PadGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                control: row.modelData.control
                                Accessible.role: Accessible.StaticText
                                Accessible.name: root.controlNames[row.modelData.control] || row.modelData.control
                                HoverHandler { id: glyphHover }
                                ToolTip.visible: glyphHover.hovered
                                ToolTip.delay: 400
                                ToolTip.text: root.controlNames[row.modelData.control] || row.modelData.control
                            }
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4
                                Repeater {
                                    model: row.modelData.inputs
                                    delegate: Rectangle {
                                        required property string modelData
                                        width: Math.max(22, cap.implicitWidth + 12)
                                        height: 22
                                        radius: Theme.rMd
                                        color: Theme.card2
                                        border.width: 1
                                        border.color: Theme.stroke2
                                        Text {
                                            id: cap
                                            anchors.centerIn: parent
                                            text: modelData
                                            color: Theme.t1
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fsMeta
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
