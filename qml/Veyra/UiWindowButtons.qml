// Minimise, maximise and close for the frameless window, as quiet 32px tool buttons.
import QtQuick
import QtQuick.Controls

Row {
    id: w
    property var shell: Window.window
    spacing: 2
    UiButton {
        kind: "tool"; size: 32; iconName: "winmin"; iconSize: 14
        tip: qsTr("réduire")
        onClicked: w.shell.showMinimized()
    }
    UiButton {
        kind: "tool"; size: 32; iconName: "winmax"; iconSize: 12
        tip: w.shell && w.shell.visibility === Window.Maximized ? qsTr("restaurer") : qsTr("agrandir")
        onClicked: w.shell.toggleMaximized()
    }
    UiButton {
        kind: "tool"; size: 32; iconName: "winclose"; iconSize: 14
        tip: qsTr("fermer")
        onClicked: w.shell.close()
    }
}
