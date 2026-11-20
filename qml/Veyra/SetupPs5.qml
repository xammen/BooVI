// PS5 setup (Connect mockup): find the console, pair once, choose how to play. The same
// bridge calls as the old three-step dialog (Ps5SourceDialog.qml). The PSN account id is
// never shown once saved: the row says it is kept and offers to change it.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: flow
    property int step: 0
    readonly property var steps: [qsTr("trouver"), qsTr("associer"), qsTr("jouer")]
    readonly property var ps5: veyra.ps5 || ({})
    readonly property bool paired: (ps5.profile || "").length > 0
    readonly property bool busy: ps5.busy === true
    readonly property string note: ps5.status || ""
    property bool editAccount: false
    signal started()
    spacing: 32
    Layout.fillWidth: true

    function done(i) { return i === 0 ? (ps5.host || "").length > 0 : i === 1 ? paired : false }
    readonly property bool canBack: step > 0
    readonly property string nextLabel: step === 2 ? qsTr("jouer") : step === 1 && !paired ? qsTr("associer") : qsTr("continuer")
    readonly property bool nextEnabled: step === 0 ? (ps5.host || "").length > 0 && !busy
                                      : step === 1 ? paired || (pin.text.length === 8 && !busy)
                                      : paired && !busy
    function opened() { veyra.ps5Load(); editAccount = false; step = paired ? 2 : 0 }
    function back() { step = Math.max(0, step - 1) }
    function next() {
        if (step === 0) { step = paired ? 2 : 1; return }
        if (step === 1) {
            if (paired) { step = 2; return }
            veyra.ps5Pair(pin.text)
            pin.text = ""
            return
        }
        veyra.ps5Play()
        started()
    }
    onPairedChanged: if (paired && step === 1) step = 2

    SetupStep {
        current: flow.step === 0
        title: qsTr("trouver ta ps5")
        text: qsTr("allume-la ou laisse-la en mode repos. BooVI la cherche sur ton réseau local.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            visible: (veyra.ps5Profiles || []).length > 0
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            Repeater {
                model: veyra.ps5Profiles || []
                delegate: UiChoice {
                    required property var modelData
                    title: "ps5"
                    detail: [String(modelData.label || ""), qsTr("déjà associée")].filter(x => x.length).join(" · ")
                    tag: flow.ps5.profile === modelData.id ? qsTr("choisie") : ""
                    tagColor: Theme.ok
                    selected: flow.ps5.profile === modelData.id
                    onClicked: veyra.ps5SelectProfile(modelData.id)
                }
            }
        }
        ColumnLayout {
            spacing: 8
            UiFieldLabel { text: (veyra.ps5Profiles || []).length > 0 ? qsTr("pas dans la liste ? adresse ip") : qsTr("adresse ip") }
            RowLayout {
                spacing: 8
                UiField {
                    objectName: "ps5-host"
                    placeholderText: "192.168.1.x"
                    text: flow.ps5.host || ""
                    onEdited: t => veyra.ps5Set("host", t)
                }
                UiButton { text: qsTr("chercher"); size: 44; enabled: !flow.busy; onClicked: veyra.ps5Scan() }
                UiButton { text: qsTr("réveiller"); size: 44; enabled: !flow.busy && flow.paired; onClicked: veyra.ps5Wake() }
            }
        }
    }

    SetupStep {
        current: flow.step === 1
        title: qsTr("associer, une seule fois")
        text: qsTr("sur la ps5 : réglages, système, lecture à distance, associer un appareil. tape le code à 8 chiffres affiché.")
        ColumnLayout {
            spacing: 8
            Layout.fillWidth: true
            UiFieldLabel { text: qsTr("compte psn") }
            Rectangle {
                visible: (flow.ps5.account || "").length > 0 && !flow.editAccount
                Layout.fillWidth: true
                implicitHeight: 48
                radius: Theme.rMd
                color: Theme.card
                border.width: 1
                border.color: Theme.stroke
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 8
                    spacing: 12
                    UiDot {}
                    Text { Layout.fillWidth: true; text: qsTr("identifiant enregistré sur ce pc"); color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsRow; elide: Text.ElideRight }
                    UiButton { kind: "tool"; size: 32; text: qsTr("changer"); onClicked: flow.editAccount = true }
                }
            }
            RowLayout {
                visible: (flow.ps5.account || "").length === 0 || flow.editAccount
                spacing: 8
                UiField {
                    objectName: "ps5-account-id"
                    placeholderText: qsTr("account id")
                    echoMode: TextInput.Password
                    onEdited: t => { if (t.length > 0) { veyra.ps5Set("account", t); flow.editAccount = false } }
                }
                UiButton { objectName: "ps5-account-lookup"; text: qsTr("trouver mon id"); size: 44; onClicked: veyra.ps5OpenAccountChecker() }
            }
        }
        ColumnLayout {
            spacing: 8
            visible: !flow.paired
            UiFieldLabel { text: qsTr("code d'association") }
            UiField {
                id: pin
                implicitWidth: 288
                fieldHeight: 56
                textSize: 22
                font.letterSpacing: 7
                placeholderText: "00000000"
                maximumLength: 8
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,8}/ }
                onAccepted: if (flow.nextEnabled) flow.next()
            }
        }
        RowLayout {
            visible: flow.paired
            spacing: 12
            UiDot {}
            Text { text: qsTr("cette ps5 est déjà associée."); color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsRow }
            UiButton { kind: "tool"; size: 32; text: qsTr("oublier"); enabled: !flow.busy; onClicked: veyra.ps5Forget() }
        }
    }

    SetupStep {
        current: flow.step === 2
        title: qsTr("comment tu joues ?")
        text: qsTr("la ps5 envoie au maximum du 1080p à 60 images par seconde. ta carte graphique fait le reste. tu pourras changer en jeu.")
        PresetChoices { sourceFps: [30, 60, 30, 60][flow.ps5.quality || 0] }
        UiCard {
            UiRow {
                first: true
                title: qsTr("regarder seulement")
                desc: qsTr("la ps5 ne reçoit aucun bouton. un badge le rappelle en jeu.")
                UiSwitch { on: flow.ps5.viewOnly === true; onSwitched: v => veyra.ps5Set("viewOnly", v) }
            }
            UiRow {
                visible: flow.ps5.pinRequired === true
                title: qsTr("code de connexion demandé")
                desc: qsTr("le code de ton compte ps5, pas celui d'association.")
                UiField { id: loginPin; implicitWidth: 120; fieldHeight: 32; echoMode: TextInput.Password; onAccepted: { veyra.ps5SendLoginPin(text); text = "" } }
                UiButton { text: qsTr("envoyer"); size: 32; onClicked: { veyra.ps5SendLoginPin(loginPin.text); loginPin.text = "" } }
            }
            UiRow {
                title: qsTr("retenir le code de connexion")
                desc: qsTr("chiffré sur ce pc et envoyé quand la ps5 le demande.")
                UiSwitch { on: veyra.preferences.ps5RememberPin === true; onSwitched: v => veyra.setPreference("ps5RememberPin", v) }
            }
        }
    }
}
