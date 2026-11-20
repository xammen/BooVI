// PC setup (Moonlight / Sunshine host): find the host, pair it with the PIN Sunshine asks
// for, then pick the game and how to play. Same model calls as the old PcSourceDialog.qml.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: flow
    property int step: 0
    readonly property var steps: [qsTr("trouver"), qsTr("associer"), qsTr("jouer")]
    readonly property var ml: veyra.moonlight
    readonly property var st: ml ? ml.state : ({})
    readonly property var cfg: st.settings || ({})
    readonly property bool paired: st.paired === true
    readonly property bool busy: st.busy === true
    readonly property string note: st.status || ""
    property int pickedApp: -1
    readonly property int app: pickedApp >= 0 ? pickedApp : (st.runningApp > 0 ? st.runningApp : (ml && ml.apps.length > 0 ? ml.apps[0].id : -1))
    property bool showStream: false
    signal started()
    spacing: 32
    Layout.fillWidth: true

    function done(i) { return i === 0 ? (st.selected || "").length > 0 : i === 1 ? paired : false }
    readonly property bool canBack: step > 0
    readonly property string nextLabel: step === 2 ? qsTr("jouer") : step === 1 && !paired ? qsTr("associer") : qsTr("continuer")
    readonly property bool nextEnabled: !busy && (step === 0 ? (st.selected || "").length > 0
                                                : step === 1 ? st.pairing !== true
                                                : paired && st.online === true && app >= 0)
    function opened() { if (ml) ml.load(); pickedApp = -1; step = paired ? 2 : 0 }
    function closed() { if (ml) ml.unload() }
    function back() { step = Math.max(0, step - 1) }
    function next() {
        if (step === 0) { step = paired ? 2 : 1; return }
        if (step === 1) { if (paired) step = 2; else ml.pair(st.selected); return }
        ml.connectStream(app)
    }
    onPairedChanged: if (paired && step === 1) step = 2
    Connections { target: flow.ml; function onStarted() { flow.started() } }

    readonly property var resolutions: [
        { id: "720p", label: "720p" }, { id: "1080p", label: "1080p" }, { id: "1440p", label: "1440p" },
        { id: "4k", label: "4k" }, { id: "native", label: qsTr("écran de ce pc") }]
    function stateText(s) {
        return s === "online" ? qsTr("en ligne") : s === "unpaired" ? qsTr("à associer") : s === "busy" ? qsTr("un jeu tourne")
             : s === "checking" ? qsTr("vérification") : qsTr("hors ligne")
    }

    SetupStep {
        current: flow.step === 0
        title: qsTr("trouver ton pc")
        text: qsTr("installe sunshine sur le pc à streamer, sur le même réseau. il apparaît ici tout seul.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            visible: flow.ml && flow.ml.hosts.length > 0
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            Repeater {
                model: flow.ml ? flow.ml.hosts : []
                delegate: UiChoice {
                    required property var modelData
                    title: String(modelData.name).toLowerCase()
                    detail: [modelData.address, (modelData.gpu || "").toLowerCase()].filter(x => x.length).join(" · ")
                    tag: flow.stateText(modelData.state)
                    tagColor: modelData.state === "online" || modelData.state === "busy" ? Theme.ok : Theme.t3
                    selected: modelData.selected === true
                    onClicked: flow.ml.select(modelData.id)
                }
            }
        }
        UiNote { busy: flow.busy; text: flow.ml && flow.ml.hosts.length === 0 ? qsTr("recherche sur le réseau") : "" }
        ColumnLayout {
            spacing: 8
            UiFieldLabel { text: qsTr("pas dans la liste ? adresse ip") }
            RowLayout {
                spacing: 8
                UiField { id: address; objectName: "moonlight-address"; placeholderText: "192.168.1.x"; onAccepted: if (text.length > 0) { flow.ml.addHost(text); text = "" } }
                UiButton { text: qsTr("ajouter"); size: 44; enabled: !flow.busy && address.text.length > 0; onClicked: { flow.ml.addHost(address.text); address.text = "" } }
                UiButton { text: qsTr("actualiser"); size: 44; enabled: !flow.busy; onClicked: flow.ml.refresh() }
            }
        }
    }

    SetupStep {
        current: flow.step === 1
        title: qsTr("associer, une seule fois")
        text: flow.st.pairing === true
              ? qsTr("ouvre sunshine sur le pc (https://ip-du-pc:47990), onglet pin, et tape ce code.")
              : qsTr("clique sur associer : BooVI affiche un code à taper dans sunshine.")
        Text {
            visible: flow.st.pairing === true
            objectName: "moonlight-pin"
            text: flow.st.pin || ""
            color: Theme.t1
            font.family: Theme.fontMono
            font.pixelSize: 40
            font.letterSpacing: 12
        }
        RowLayout {
            visible: flow.paired
            spacing: 12
            UiDot {}
            Text { text: qsTr("ce pc est déjà associé."); color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsRow }
            UiButton { kind: "tool"; size: 32; text: qsTr("oublier"); enabled: !flow.busy; onClicked: flow.ml.forget(flow.st.selected) }
        }
    }

    SetupStep {
        current: flow.step === 2
        title: qsTr("comment tu joues ?")
        text: flow.st.online === true ? qsTr("choisis le jeu, puis le réglage. tu pourras changer en jeu.")
                                      : qsTr("le pc ne répond pas : allume-le ou réveille-le, puis actualise.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: flow.st.online === true && flow.ml && flow.ml.apps.length > 0
            UiFieldLabel { text: qsTr("jeu") }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: flow.ml ? flow.ml.apps : []
                    delegate: UiButton {
                        required property var modelData
                        text: String(modelData.name).toLowerCase() + (modelData.running ? " · " + qsTr("en cours") : "")
                        size: 36
                        active: flow.app === modelData.id
                        onClicked: flow.pickedApp = modelData.id
                    }
                }
            }
        }
        UiButton { visible: flow.st.online !== true; text: qsTr("actualiser"); size: 40; enabled: !flow.busy; onClicked: flow.ml.refresh() }
        PresetChoices { sourceFps: flow.cfg.fps || 60 }
        UiButton {
            text: flow.showStream ? qsTr("masquer les réglages du flux") : qsTr("réglages du flux")
            kind: "tool"
            size: 32
            iconName: "down"
            onClicked: flow.showStream = !flow.showStream
        }
        UiCard {
            visible: flow.showStream
            UiRow {
                first: true
                title: qsTr("résolution")
                desc: qsTr("ce que le pc encode. l'upscaling se fait ici, après.")
                UiSeg { options: flow.resolutions; current: flow.cfg.res || "1080p"; onPicked: id => flow.ml.set("res", id) }
            }
            UiRow {
                title: qsTr("images par seconde")
                UiSeg { options: [30, 60, 90, 120, 144].map(v => ({ id: String(v), label: String(v) })); current: String(flow.cfg.fps || 60); onPicked: id => flow.ml.set("fps", Number(id)) }
            }
            UiRow {
                title: qsTr("débit max")
                desc: qsTr("en filaire, 300 à 500 Mb/s passent. baisse en wifi si ça saccade.")
                UiSeg { options: [50, 150, 300, 500].map(v => ({ id: String(v), label: v + (v === 500 ? " Mb/s" : "") })); current: String(flow.cfg.bitrate || 150); onPicked: id => flow.ml.set("bitrate", Number(id)) }
            }
            UiRow {
                title: qsTr("codec")
                UiSeg { options: [{ id: "0", label: qsTr("auto") }, { id: "1", label: "h.264" }, { id: "2", label: "hevc" }, { id: "3", label: "av1" }]; current: String(flow.cfg.codec || 0); onPicked: id => flow.ml.set("codec", Number(id)) }
            }
            UiRow {
                title: "hdr"
                desc: flow.st.hdrHost ? qsTr("l'écran du pc doit être en hdr, codec hevc ou av1.") : qsTr("ce pc n'annonce pas d'encodage 10 bits.")
                UiSwitch { enabled: !!flow.st.hdrHost || flow.cfg.hdr === true; on: flow.cfg.hdr === true; onSwitched: v => flow.ml.set("hdr", v) }
            }
            UiRow {
                title: qsTr("manette")
                desc: qsTr("ta manette devient la manette xbox 1 du pc.")
                UiSwitch { on: flow.cfg.gamepad !== false; onSwitched: v => flow.ml.set("gamepad", v) }
            }
            UiRow {
                title: qsTr("capturer clavier et souris au lancement")
                desc: qsTr("ctrl+alt+maj+z libère, +q coupe le flux.")
                UiSwitch { on: flow.cfg.captureInput !== false; onSwitched: v => flow.ml.set("captureInput", v) }
            }
        }
    }
}
