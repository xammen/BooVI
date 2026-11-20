// Capture card setup: the device and its mode, the sound, then how to play. Generic UVC /
// DirectShow choices only; brand drivers, colour range and buffering stay in the full
// capture dialog behind "réglages avancés" (CaptureSourceDialog.qml).
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: flow
    property int step: 0
    readonly property var steps: [qsTr("appareil"), qsTr("son"), qsTr("jouer")]
    readonly property bool busy: veyra.captureQueryBusy === true
    // The bridge's capture status is about picking a format: it belongs to the first step.
    readonly property string note: step === 0 ? veyra.captureStatus || "" : ""
    signal started()
    signal requestAdvanced()
    spacing: 32
    Layout.fillWidth: true

    function done(i) { return i === 0 ? veyra.captureDeviceId.length > 0 && veyra.captureResolutionId.length > 0 : i === 1 ? veyra.captureAudioChoice !== undefined : false }
    readonly property bool canBack: step > 0
    readonly property string nextLabel: step === 2 ? qsTr("jouer") : qsTr("continuer")
    readonly property bool nextEnabled: !busy && (step === 0 ? veyra.captureDeviceId.length > 0 : true)
    function opened() { veyra.refreshCaptureDevices(); step = veyra.hasCaptureSession ? 2 : 0 }
    function back() { step = Math.max(0, step - 1) }
    function next() {
        if (step < 2) { step += 1; return }
        if (veyra.startCaptureSession()) started()
    }

    SetupStep {
        current: flow.step === 0
        title: qsTr("brancher la carte")
        text: qsTr("la sortie hdmi de la console va dans la carte, la carte dans ce pc en usb ou pcie.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            Repeater {
                model: veyra.captureDevices || []
                delegate: UiChoice {
                    required property var modelData
                    title: String(modelData.label).toLowerCase()
                    selected: veyra.captureDeviceId === modelData.id
                    tag: selected ? qsTr("choisie") : ""
                    tagColor: Theme.ok
                    onClicked: veyra.captureDeviceId = modelData.id
                }
            }
        }
        UiNote {
            busy: flow.busy
            text: flow.busy ? qsTr("lecture des formats") : (veyra.captureDevices || []).length === 0 ? qsTr("aucune carte détectée") : ""
        }
        RowLayout {
            spacing: 24
            ColumnLayout {
                spacing: 8
                UiFieldLabel { text: qsTr("résolution") }
                UiSelect {
                    objectName: "capture-resolution"
                    implicitWidth: 200
                    enabled: !flow.busy && veyra.captureDeviceId.length > 0
                    options: veyra.captureResolutionOptions || []
                    current: veyra.captureResolutionId
                    onPicked: id => veyra.captureResolutionId = id
                }
            }
            ColumnLayout {
                spacing: 8
                UiFieldLabel { text: qsTr("images par seconde") }
                UiSelect {
                    objectName: "capture-frame-rate"
                    implicitWidth: 160
                    enabled: !flow.busy && veyra.captureResolutionId.length > 0
                    options: veyra.captureFrameRateOptions || []
                    current: veyra.captureFrameRateId
                    onPicked: id => veyra.captureFrameRateId = id
                }
            }
            UiButton { Layout.alignment: Qt.AlignBottom; text: qsTr("actualiser"); size: 32; enabled: !flow.busy; onClicked: veyra.refreshCaptureDevices() }
        }
    }

    SetupStep {
        current: flow.step === 1
        title: qsTr("le son")
        text: qsTr("le son arrive par la carte. choisis son entrée pour l'entendre et l'enregistrer.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            Repeater {
                model: veyra.captureAudioInputs || []
                delegate: UiChoice {
                    required property var modelData
                    title: String(modelData.label).toLowerCase()
                    selected: String(veyra.captureAudioChoice) === String(modelData.id)
                    onClicked: veyra.captureAudioChoice = Number(modelData.id)
                }
            }
        }
    }

    SetupStep {
        current: flow.step === 2
        title: qsTr("comment tu joues ?")
        text: qsTr("la carte envoie ce que la console sort. ta carte graphique fait le reste. tu pourras changer en jeu.")
        RowLayout {
            visible: veyra.captureSignalText.length > 0
            spacing: 12
            UiDot { hollow: veyra.captureSignalLevel !== "ok" && veyra.captureSignalLevel !== "warn"; tone: veyra.captureSignalLevel === "warn" ? Theme.warn : Theme.ok }
            Text { text: qsTr("signal : %1").arg(veyra.captureSignalText.toLowerCase()); color: Theme.t2; font.family: Theme.fontMono; font.pixelSize: Theme.fsMeta }
        }
        PresetChoices { sourceFps: Number(veyra.captureRequestedFps) > 0 ? Number(veyra.captureRequestedFps) : 60 }
        UiButton {
            text: qsTr("réglages avancés de la carte")
            kind: "tool"
            size: 32
            iconName: "settings"
            onClicked: flow.requestAdvanced()
        }
    }
}
