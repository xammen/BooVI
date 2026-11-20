// Dialog routing and scrim. Each dialog owns its own editable content.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: host
    // Compatibility type used by the professional and node views.
    component VTextField: DialogTextField {}
    anchors.fill: parent
    z: 100

    // Active dialog: network/capture/ps5/moonlight/xbox/screen/subtitle/audio/save/manage.
    property string dialog: ""
    onDialogChanged: if (dialog !== "capture") legacyCapturePicker.close()

    function open(key) {
        veyra.logUi("ui-dialog", "open " + key + " (was " + (host.dialog || "none") + ")")
        host.dialog = key
    }
    function close() { host.dialog = "" }

    signal startCapture()
    signal startPs5()
    signal startMoonlight()
    signal startXbox()
    signal startScreen()

    CaptureLegacyPicker {
        id: legacyCapturePicker
        formats: visible ? veyra.captureFormats : []
        selectedKey: veyra.captureFormatKey
        onSelected: key => { if (!veyra.captureQueryBusy) veyra.captureFormatKey = key }
    }
    Connections {
        target: veyra
        function onCaptureChanged() { if (veyra.captureQueryBusy) legacyCapturePicker.close() }
    }

    // Scrim (M16): the design dims the page behind the dialog, opacity .2s linear.
    // The native video window cannot be dimmed from QML; only the dialog panel is cut
    // out of it (videoCover below), so the picture beside the dialog stays undimmed.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)
        opacity: host.dialog !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.d(200) } }
        TapHandler { onTapped: host.close() }
    }
    // For the motion probe: the open dialog's scale.
    property real motionScale: 1

    // Fork: only the full capture dialog is left here, as the "réglages avancés" of a
    // capture card (brand drivers, colour range, buffering). The source setups live on the
    // consoles page; the other dialogs are no longer routed.
    CaptureSourceDialog {
        id: captureDialog
        dialogHost: host
        legacyPicker: legacyCapturePicker
    }
}
