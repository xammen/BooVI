// Consoles (Connect mockup): pick the source at the top, follow its three steps on the left
// rail, act with retour / continuer at the bottom. Each source owns its flow file
// (SetupPs5, SetupXbox, SetupPc, SetupCapture); this page only frames the current one.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

VPage {
    id: root
    signal requestPage(string page)
    signal requestAdvancedCapture()
    dragStripHeight: 0
    property string source: "ps5"
    readonly property var flows: ({ ps5: ps5Flow, xbox: xboxFlow, pc: pcFlow, capture: captureFlow, join: joinFlow })
    readonly property Item flow: flows[source]

    // Opens a source on the step its state calls for (a paired PS5 lands on "jouer").
    function show(id, step) {
        if (flow && flow.closed) flow.closed()
        source = id.length > 0 ? id : source
        flow.opened()
        if (step !== undefined && step >= 0) flow.step = step
        Qt.callLater(() => root.noteBaseline = root.flow.note)
    }
    // The bridge keeps its last status line; the page shows only what changed since it
    // opened, so a stale "saved host loaded" never reads as an instruction.
    property string noteBaseline: ""
    function started() { requestPage("min") }
    // The step's actions sit under the step; when the step is taller than the page they pin
    // to a footer instead, so the primary action never scrolls away. The in-flow copy keeps
    // its space either way, so the switch cannot change the height it depends on.
    readonly property bool overflows: scroller.contentHeight > scroller.height
    component StepActions: RowLayout {
        spacing: 8
        UiButton {
            visible: root.flow.canBack
            text: qsTr("retour")
            size: 48
            fontSize: Theme.fsCtl
            onClicked: root.flow.back()
        }
        UiButton {
            objectName: "connect-next"
            kind: "solid"
            size: 48
            text: root.flow.nextLabel
            iconName: root.flow.step === 2 ? "play" : ""
            iconFilled: true
            iconSize: 12
            hoverNudge: root.flow.step === 2 ? 2 : 0
            enabled: root.flow.nextEnabled
            onClicked: root.flow.next()
        }
    }

    Flickable {
        id: scroller
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: frame.implicitHeight + frame.y + 48
        bottomMargin: root.overflows ? footer.height - 80 : 0
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: VScrollBar {}

        ColumnLayout {
            id: frame
            width: Math.min(960, scroller.width - 64)
            x: Math.round((scroller.width - width) / 2)
            y: Math.round(Math.max(32, Math.min(64, scroller.height * 0.08)))
            spacing: 48

            RowLayout {
                Layout.fillWidth: true
                spacing: 24
                UiFieldLabel { text: qsTr("source") }
                UiSeg {
                    objectName: "connect-source"
                    accessibleName: qsTr("source")
                    cellHeight: 32
                    options: [{ id: "ps5", label: "ps5" }, { id: "xbox", label: "xbox" }, { id: "pc", label: "pc" }, { id: "capture", label: qsTr("carte de capture") }].concat(veyra.join ? [{ id: "join", label: qsTr("partage") }] : [])
                    current: root.source
                    onPicked: id => root.show(id)
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 64

                Item {
                    id: rail
                    Layout.alignment: Qt.AlignTop
                    implicitWidth: 192
                    implicitHeight: 64 * 3
                    Accessible.name: qsTr("étapes")
                    Rectangle { width: 1; height: parent.height; color: Theme.line }
                    Rectangle {
                        width: 2
                        height: 64
                        radius: 1
                        color: Theme.t1
                        y: root.flow.step * 64
                        Behavior on y { NumberAnimation { duration: Theme.d(Theme.durGlide); easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.ease } }
                    }
                    Column {
                        anchors.fill: parent
                        Repeater {
                            model: root.flow.steps
                            delegate: AbstractButton {
                                id: stepItem
                                required property string modelData
                                required property int index
                                readonly property bool here: index === root.flow.step
                                readonly property bool isDone: !here && root.flow.done(index)
                                width: rail.width
                                height: 64
                                hoverEnabled: true
                                enabled: index <= root.flow.step || root.flow.done(index - 1)
                                Accessible.name: modelData
                                onClicked: root.flow.step = index
                                background: Rectangle {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    radius: Theme.rMd
                                    color: "transparent"
                                    border.width: stepItem.visualFocus ? 2 : 0
                                    border.color: Theme.focusRing
                                }
                                contentItem: Column {
                                    leftPadding: 24
                                    topPadding: 13
                                    spacing: 4
                                    Text {
                                        text: "0" + (stepItem.index + 1) + (stepItem.isDone ? qsTr(" · fait") : "")
                                        color: stepItem.here || stepItem.isDone ? Theme.t2 : Theme.t3
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fsLabel
                                        font.letterSpacing: 0.8
                                        Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                                    }
                                    Text {
                                        text: stepItem.modelData
                                        color: stepItem.here ? Theme.t1 : stepItem.isDone || stepItem.hovered ? Theme.t2 : Theme.t3
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fsRow
                                        Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                                    }
                                }
                                HoverHandler { cursorShape: stepItem.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            }
                        }
                    }
                }

                ColumnLayout {
                    id: column
                    Layout.fillWidth: true
                    Layout.maximumWidth: 600
                    Layout.alignment: Qt.AlignTop
                    spacing: 32
                    SetupPs5 { id: ps5Flow; visible: root.source === "ps5"; onStarted: root.started() }
                    SetupXbox { id: xboxFlow; visible: root.source === "xbox"; onStarted: root.started() }
                    SetupPc { id: pcFlow; visible: root.source === "pc"; onStarted: root.started() }
                    SetupJoin { id: joinFlow; visible: root.source === "join"; onStarted: root.started() }
                    SetupCapture { id: captureFlow; visible: root.source === "capture"; onStarted: root.started(); onRequestAdvanced: root.requestAdvancedCapture() }
                    UiNote {
                        busy: root.flow.busy
                        text: root.flow.note !== root.noteBaseline ? root.flow.note : ""
                    }
                    StepActions {
                        opacity: root.overflows ? 0 : 1
                        enabled: !root.overflows
                    }
                }
            }
        }
    }

    // The step's actions stay on screen whatever the step's length: a footer aligned with
    // the content column, over a short fade of the page colour.
    Item {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 96
        visible: root.overflows
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(Theme.bgOuter.r, Theme.bgOuter.g, Theme.bgOuter.b, 0) }
                GradientStop { position: 0.35; color: Theme.bgOuter }
            }
        }
        StepActions {
            x: frame.x + rail.implicitWidth + 64
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
        }
    }
}
