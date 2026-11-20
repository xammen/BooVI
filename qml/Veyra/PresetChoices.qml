// The "how do you play" list of a source's last step: the three presets as radio rows.
// Picking one applies it at once (StreamPresets.apply); the stream toolbar shows the same.
import QtQuick
import QtQuick.Layouts
import QtQuick.Window

ColumnLayout {
    id: list
    // The source's nominal frame rate, for the fluidity estimate.
    property real sourceFps: 60
    spacing: 0
    Layout.fillWidth: true
    Accessible.role: Accessible.List
    StreamPresets { id: presets }
    readonly property int screenHeight: Math.round(Screen.height * Screen.devicePixelRatio)
    readonly property string target: presets.targetLabel(presets.targetForHeight(screenHeight))
    readonly property int fluidFps: {
        const hz = veyra.displayRefreshHz
        const raw = sourceFps * presets.fluidMultiplier
        return Math.round(veyra.outputRateMode === 1 && hz > 1 ? Math.min(raw, hz) : raw)
    }

    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
    Repeater {
        model: [
            { id: "performance", desc: qsTr("image native, pas de frame gen. la latence la plus basse, pour les jeux compétitifs."), tag: qsTr("latence min") },
            { id: "balanced", desc: qsTr("super résolution vers %1, sans frame gen. le bon réglage par défaut.").arg(list.target), tag: qsTr("conseillé") },
            { id: "fluid", desc: qsTr("super résolution et frame gen x%1, jusqu'à %2 images par seconde. chaque vraie image attend un peu plus.").arg(presets.fluidMultiplier).arg(list.fluidFps), tag: qsTr("fluidité max") }
        ]
        delegate: UiChoice {
            required property var modelData
            title: presets.options.find(o => o.id === modelData.id).label
            desc: modelData.desc
            tag: modelData.tag
            selected: presets.current === modelData.id
            onClicked: presets.apply(modelData.id, list.screenHeight)
        }
    }
}
