// The stream page: the picture, edge to edge. Main.qml sizes the window to the stream's
// aspect and places the native video window over this page; the floating controls live in
// StreamOverlay.qml. This page only draws what sits under the picture: black, the
// "connecting" pill while a source opens, and the preview's dither when there is no stream.
import QtQuick
import QtQuick.Layouts

VPage {
    id: root
    signal requestPage(string page)
    signal requestAspect(real aspect)
    signal requestFullscreen()
    property bool preview: false
    dragStripHeight: 0

    // cineIn: the stage opens from inset(12% 0) to inset(0) over 800 ms. The picture is a
    // native window, so main.cpp reads this carrier and insets the video window's region.
    Item {
        id: cineIn
        objectName: "videoInset"
        property real frac: 0
        visible: false
        width: 0
        height: 0
    }
    NumberAnimation {
        id: cineInAnim
        target: cineIn
        property: "frac"
        to: 0
        duration: Theme.d(800)
        easing.type: Easing.BezierSpline
        easing.bezierCurve: Theme.ease
    }
    onEnter: { cineIn.frac = Theme.reduced ? 0 : 0.12; cineInAnim.restart() }

    readonly property real pictureHeight: parent ? parent.height : 0

    Rectangle {
        id: stage
        anchors.fill: parent
        color: root.preview ? Theme.bgStage : Theme.videoBlack

        UiDither {
            visible: root.preview
            anchors.fill: parent
            mode: "stage"
        }
        Text {
            visible: root.preview
            anchors.centerIn: parent
            text: qsTr("APERÇU · AUCUN FLUX")
            color: Theme.t3
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsMeta
            font.letterSpacing: 0.9
        }
        // A stream still connecting: a pill in the middle of the black picture.
        Rectangle {
            objectName: "min-live-opening"
            readonly property bool videoCover: true
            property real coverRadius: Theme.rLg
            visible: veyra.liveOpeningText.length > 0 && !root.preview
            anchors.centerIn: parent
            width: openingRow.implicitWidth + 32
            height: 40
            radius: Theme.rLg
            color: Theme.card
            border.width: 1
            border.color: Theme.stroke
            RowLayout {
                id: openingRow
                anchors.centerIn: parent
                spacing: 12
                UiNote { busy: true; Layout.fillWidth: false }
                Text {
                    text: veyra.liveOpeningText.toLowerCase()
                    color: Theme.t1
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMeta
                }
            }
        }
    }

    onPictureHeightChanged: if (veyra.previewAspect > 0.2) root.requestAspect(veyra.previewAspect)
}
