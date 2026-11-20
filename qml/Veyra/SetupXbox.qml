// Xbox setup: sign in with the Microsoft account (device code), pick the console, then how
// to play. Same model calls as the old XboxSourceDialog.qml.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: flow
    property int step: 0
    readonly property var steps: [qsTr("compte"), qsTr("console"), qsTr("jouer")]
    readonly property var xb: veyra.xbox
    readonly property var st: xb ? xb.state : ({})
    readonly property bool signedIn: st.signedIn === true
    readonly property bool busy: st.busy === true
    readonly property string note: st.status || ""
    signal started()
    spacing: 32
    Layout.fillWidth: true

    function done(i) { return i === 0 ? signedIn : i === 1 ? (st.selected || "").length > 0 : false }
    readonly property bool canBack: step > 0
    readonly property string nextLabel: step === 2 ? qsTr("jouer") : step === 0 && !signedIn ? qsTr("se connecter") : qsTr("continuer")
    readonly property bool nextEnabled: !busy && (step === 0 ? true : step === 1 ? (st.selected || "").length > 0 : signedIn && (st.selected || "").length > 0)
    function opened() { if (xb) xb.load(); step = signedIn ? ((st.selected || "").length > 0 ? 2 : 1) : 0 }
    function back() { step = Math.max(0, step - 1) }
    function next() {
        if (step === 0) { if (signedIn) step = 1; else xb.signIn(); return }
        if (step === 1) { step = 2; return }
        xb.connectStream()
    }
    onSignedInChanged: if (signedIn && step === 0) step = 1
    Connections { target: flow.xb; function onStarted() { flow.started() } }

    SetupStep {
        current: flow.step === 0
        title: qsTr("relier ton compte")
        text: qsTr("connexion sur la page de microsoft : BooVI ne voit jamais ton mot de passe. non officiel, ça peut casser sans prévenir.")
        ColumnLayout {
            visible: (flow.st.code || "").length > 0
            spacing: 12
            UiFieldLabel { text: qsTr("code à taper sur microsoft.com/link") }
            Text {
                objectName: "xbox-code"
                text: flow.st.code || ""
                color: Theme.t1
                font.family: Theme.fontMono
                font.pixelSize: 40
                font.letterSpacing: 6
            }
            UiButton { text: qsTr("ouvrir la page"); onClicked: flow.xb.openSignInPage() }
        }
        RowLayout {
            visible: flow.signedIn
            spacing: 12
            UiDot {}
            Text { text: qsTr("compte relié, gardé chiffré sur ce pc."); color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsRow }
            UiButton { kind: "tool"; size: 32; text: qsTr("se déconnecter"); enabled: !flow.busy; onClicked: flow.xb.signOut() }
        }
    }

    SetupStep {
        current: flow.step === 1
        title: qsTr("choisir la console")
        text: qsTr("même compte sur la xbox, et fonctions à distance activées dans appareils et connexions.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            Repeater {
                model: flow.signedIn && flow.xb ? flow.xb.consoles : []
                delegate: UiChoice {
                    required property var modelData
                    title: String(modelData.name).toLowerCase()
                    detail: [String(modelData.type || "").toLowerCase(), String(modelData.power || "").toLowerCase()].filter(x => x.length).join(" · ")
                    selected: flow.st.selected === modelData.id
                    onClicked: flow.xb.select(modelData.id)
                }
            }
        }
        RowLayout {
            spacing: 8
            UiButton { text: qsTr("actualiser"); enabled: !flow.busy; onClicked: flow.xb.refresh() }
        }
    }

    SetupStep {
        current: flow.step === 2
        title: qsTr("comment tu joues ?")
        text: qsTr("la xbox envoie du 1080p. ta carte graphique fait le reste. tu pourras changer en jeu.")
        PresetChoices {}
        UiCard {
            UiRow {
                first: true
                title: qsTr("manette")
                desc: qsTr("ta manette pilote la xbox.")
                UiSwitch { on: flow.st.gamepad !== false; onSwitched: v => flow.xb.set("gamepad", v) }
            }
        }
    }
}
