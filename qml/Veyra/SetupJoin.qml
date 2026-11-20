// Rejoindre un partage: a friend shares their stream from BooVI and sends a link; this PC
// joins with its own BooVI, so the super resolution and frame generation run on this GPU.
// Paste the link, pick how to play, join. The link stays in memory for this run only.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: flow
    property int step: 0
    readonly property var steps: [qsTr("le lien"), qsTr("jouer")]
    readonly property var jm: veyra.join
    readonly property var st: jm ? jm.state : ({})
    readonly property bool linkValid: st.linkValid === true
    readonly property bool busy: st.phase === "connecting"
    readonly property string note: st.phase === "failed" || st.phase === "ended" ? (st.message || "") : ""
    signal started()
    spacing: 32
    Layout.fillWidth: true

    function done(i) { return i === 0 ? linkValid : false }
    readonly property bool canBack: step > 0
    readonly property string nextLabel: step === 1 ? qsTr("rejoindre") : qsTr("continuer")
    readonly property bool nextEnabled: !!jm && linkValid && !busy
    function opened() { if (jm && !linkValid) jm.pasteLink(); step = linkValid ? 1 : 0 }
    function back() { step = 0 }
    function next() {
        if (step === 0) { step = 1; return }
        jm.join()
    }
    Connections { target: flow.jm; function onStarted() { flow.started() } }

    SetupStep {
        current: flow.step === 0
        title: qsTr("coller le lien de ton ami")
        text: qsTr("il partage depuis BooVI et t'envoie un lien. ici, c'est ta carte graphique qui fait la super résolution et la frame gen, à la fréquence de ton écran.")
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            UiField {
                id: linkField
                objectName: "join-link"
                Layout.fillWidth: true
                placeholderText: "https://…/?t=…"
                echoMode: TextInput.Password
                onTextChanged: if (flow.jm) flow.jm.setLink(text)
            }
            UiButton {
                text: qsTr("coller")
                size: 44
                onClicked: { linkField.text = ""; linkField.paste() }
            }
        }
        RowLayout {
            visible: (flow.st.saved || "").length > 0
            spacing: 12
            Text {
                text: qsTr("gardé : %1").arg(flow.st.saved || "")
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
            }
            UiButton { text: qsTr("oublier"); size: 32; onClicked: flow.jm.forgetSaved() }
        }
        RowLayout {
            spacing: 12
            UiDot { hollow: !flow.linkValid }
            Text {
                text: flow.linkValid ? (flow.st.access === true ? qsTr("accès personnel · hôte %1 · gardé sur ce pc après la première partie").arg(flow.st.host || "")
                                                                 : qsTr("lien reconnu · hôte %1").arg(flow.st.host || ""))
                                     : qsTr("le lien contient ?t= ou ?a= suivi d'un code")
                color: Theme.t2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMeta
            }
        }
    }

    SetupStep {
        current: flow.step === 1
        title: qsTr("comment tu joues ?")
        text: qsTr("l'hôte t'envoie ses vraies images. la frame gen et la super résolution tournent ici : dlss sur rtx 40 et 50, fsr 3.1 sur toute carte dx12. tu pourras changer en jeu.")
        PresetChoices {}
        UiCard {
            UiRow {
                first: true
                title: qsTr("manette")
                desc: qsTr("ta manette joue chez ton ami : une dualsense envoie aussi le gyroscope et le pavé tactile, et reçoit les vibrations.")
            }
        }
    }
}
