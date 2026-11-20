// ps5 dialog (fork): three steps, find -> pair -> play. Upstream showed host, pairing,
// stream settings and the login PIN as one 14-row form; here each step shows what it needs,
// pairing is skipped once the host is paired, stream settings sit under Advanced, and the
// login PIN row appears only while the console asks for it. Same bridge calls as upstream.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

DialogLayer {
    id: ps5Dialog
    objectName: "ps5-dialog"
    key: "ps5"
    glyph: "gamepad"
    title: qsTr("PS5 串流")
    sub: qsTr("局域网 Remote Play · 凭据加密保存在本机")
    dialogWidth: 640

    readonly property var ps5: veyra.ps5 || ({})
    readonly property bool paired: (ps5.profile || "").length > 0
    readonly property bool active: ps5.active === true
    // 0 find, 1 pair, 2 play. Opening the dialog lands on play when a host is paired.
    property int step: 0
    property bool advanced: false
    readonly property var ease: [0.2, 0, 0, 1, 1, 1]

    readonly property string labelNext: qsTr("下一步")
    readonly property string labelBack: qsTr("上一步")
    readonly property string labelCancel: qsTr("取消")
    readonly property string labelDisconnect: qsTr("断开")
    readonly property string labelConnect: qsTr("连接")
    readonly property string labelReconnect: qsTr("应用设置并重连")

    actions: step === 2
        ? [{ label: active ? labelDisconnect : labelCancel },
           { label: active ? labelReconnect : labelConnect, primary: true }]
        : step === 1
            ? [{ label: labelBack }]
            : [{ label: labelCancel }, { label: labelNext, primary: true }]

    onActionTriggered: label => {
        if (label === labelBack) { step = 0; return }
        if (label === labelNext) { step = paired ? 2 : 1; return }
        if (label === labelDisconnect) { veyra.ps5Cancel(); return }
        if (label === labelCancel) { if (veyra.ps5.busy) veyra.ps5Cancel(); else dialogHost.close(); return }
        // Play wakes a console in rest mode, then connects (fork).
        veyra.ps5Play(); dialogHost.close(); dialogHost.startPs5()
    }
    Connections {
        target: dialogHost
        function onDialogChanged() {
            if (dialogHost.dialog !== "ps5") return
            veyra.ps5Load()
            ps5Dialog.advanced = false
            ps5Dialog.step = ps5Dialog.paired ? 2 : 0
        }
    }
    // Pairing succeeded: go straight to the play step.
    onPairedChanged: if (paired && step === 1) step = 2

    readonly property var qualities: [{ id: "0", label: "720p · 30 fps" }, { id: "1", label: "720p · 60 fps" }, { id: "2", label: "1080p · 30 fps" }, { id: "3", label: "1080p · 60 fps" }]
    readonly property var bitrates: [5, 10, 15, 20, 30, 50, 80, 100].map((m, i) => ({ id: String(i), label: m + " Mbps" }))
    readonly property var codecs: ["H.264", "H.265", "H.265 HDR"]

    // --- stepper: three labels and a bar that slides under the current one ------------
    Item {
        Layout.fillWidth: true
        implicitHeight: 40
        Row {
            id: stepRow
            anchors.left: parent.left
            anchors.right: parent.right
            Repeater {
                model: [qsTr("查找主机"), qsTr("配对"), qsTr("开玩")]
                delegate: AbstractButton {
                    required property int index
                    required property string modelData
                    width: stepRow.width / 3
                    height: 36
                    // Pairing can be revisited; play needs a paired host.
                    enabled: index !== 2 || ps5Dialog.paired
                    onClicked: ps5Dialog.step = index
                    Accessible.name: modelData
                    contentItem: Row {
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: index < ps5Dialog.step || (index === 1 && ps5Dialog.paired) ? "✓" : "0" + (index + 1)
                            color: index === ps5Dialog.step ? Theme.accent : (index < ps5Dialog.step || (index === 1 && ps5Dialog.paired) ? Theme.ok : Theme.t3)
                            font.family: Theme.fontMono
                            font.pixelSize: 11
                            Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: index === ps5Dialog.step ? Theme.t1 : Theme.t2
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody + 1
                            Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                        }
                    }
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.stroke
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width / 3
            height: 2
            radius: 1
            color: Theme.accent
            x: ps5Dialog.step * width
            Behavior on x { NumberAnimation { duration: Theme.d(300); easing.type: Easing.BezierSpline; easing.bezierCurve: ps5Dialog.ease } }
        }
    }

    // --- step 1: find --------------------------------------------------------------------
    ColumnLayout {
        visible: ps5Dialog.step === 0
        Layout.fillWidth: true
        spacing: 12
        DialogSection { text: qsTr("找到你的 PS5") }
        Text {
            Layout.fillWidth: true
            text: qsTr("开机或待机都可以，BooVI 会在局域网里查找。")
            color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsBody; wrapMode: Text.Wrap
        }
        DialogGroup {
            VRow {
                visible: veyra.ps5Profiles.length > 0
                label: qsTr("已保存主机")
                VSelect {
                    objectName: "ps5-profile"
                    implicitWidth: 220
                    value: { const p = veyra.ps5Profiles.find(x => x.id === ps5Dialog.ps5.profile); return p ? p.label : qsTr("请选择") }
                    options: veyra.ps5Profiles
                    onPicked: id => veyra.ps5SelectProfile(id)
                }
            }
            VRow {
                label: qsTr("主机地址")
                hint: qsTr("PS5 设置 → 网络 → 连接状态；找不到时可手填 IP")
                RowLayout {
                    spacing: 6
                    DialogTextField { objectName: "ps5-host"; implicitWidth: 170; placeholder: "192.168.1.x"; text: ps5Dialog.ps5.host || ""; onEdited: text => veyra.ps5Set("host", text) }
                    VSpinner { visible: ps5Dialog.ps5.busy === true; Layout.alignment: Qt.AlignVCenter }
                    VButton { text: qsTr("查找"); ghost: true; enabled: !ps5Dialog.ps5.busy; onClicked: veyra.ps5Scan() }
                    VButton { text: qsTr("唤醒"); ghost: true; enabled: !ps5Dialog.ps5.busy && ps5Dialog.paired; onClicked: veyra.ps5Wake() }
                }
            }
        }
    }

    // --- step 2: pair --------------------------------------------------------------------
    ColumnLayout {
        visible: ps5Dialog.step === 1
        Layout.fillWidth: true
        spacing: 12
        DialogSection { text: qsTr("配对一次即可") }
        Text {
            Layout.fillWidth: true
            text: qsTr("PS5：设置 → 系统 → 远程游玩 → 关联设备，然后输入显示的 8 位数字。")
            color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsBody; wrapMode: Text.Wrap
        }
        DialogGroup {
            VRow {
                label: "PSN Account ID"
                hint: qsTr("点击获取 Account ID，在网页查询后复制到这里。支持数字 ID 或 8 字节 Base64，不是 PSN 昵称。")
                hintBelow: true
                RowLayout {
                    spacing: 6
                    DialogTextField { objectName: "ps5-account-id"; implicitWidth: 220; text: ps5Dialog.ps5.account || ""; onEdited: text => veyra.ps5Set("account", text) }
                    VButton { objectName: "ps5-account-lookup"; text: qsTr("获取 Account ID"); ghost: true; onClicked: veyra.ps5OpenAccountChecker() }
                }
            }
            VRow {
                label: qsTr("8 位配对码")
                RowLayout {
                    spacing: 6
                    DialogTextField { id: pairPin; implicitWidth: 140; placeholder: "00000000" }
                    VButton { text: qsTr("配对并保存"); primary: true; enabled: !ps5Dialog.ps5.busy && pairPin.text.length === 8; onClicked: { veyra.ps5Pair(pairPin.text); pairPin.text = "" } }
                }
            }
            VRow {
                visible: ps5Dialog.paired
                label: qsTr("删除配对")
                hint: qsTr("只删除本机保存的这台主机；以后需要重新配对")
                VButton { text: qsTr("删除"); ghost: true; enabled: !ps5Dialog.ps5.busy; onClicked: veyra.ps5Forget() }
            }
        }
    }

    // --- step 3: play --------------------------------------------------------------------
    ColumnLayout {
        visible: ps5Dialog.step === 2
        Layout.fillWidth: true
        spacing: 12
        DialogSection { text: qsTr("准备开玩") }
        Text {
            Layout.fillWidth: true
            text: qsTr("PS5 最高发送 1080p 60 fps，显卡负责其余部分。")
            color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsBody; wrapMode: Text.Wrap
        }
        DialogGroup {
            VRow {
                label: (ps5Dialog.ps5.host || "") + " · " + qsTr("已配对")
                hint: ps5Dialog.qualities[ps5Dialog.ps5.quality || 0].label + " · " + ps5Dialog.codecs[ps5Dialog.ps5.codec || 0]
                      + " · " + ps5Dialog.bitrates[ps5Dialog.ps5.bitrate || 0].label
                VButton { text: qsTr("高级设置"); ghost: true; onClicked: ps5Dialog.advanced = !ps5Dialog.advanced }
            }
            VRow {
                label: qsTr("仅观看")
                hint: qsTr("不向 PS5 转发电脑手柄输入")
                VSwitch { checked: ps5Dialog.ps5.viewOnly === true; onToggled: checked => veyra.ps5Set("viewOnly", checked) }
            }
            VRow {
                visible: ps5Dialog.ps5.pinRequired === true
                label: qsTr("PS5 要求输入登录 PIN")
                hint: qsTr("不是 8 位配对码")
                RowLayout {
                    spacing: 6
                    DialogTextField { id: loginPin; implicitWidth: 120 }
                    VButton { text: qsTr("提交"); primary: true; onClicked: { veyra.ps5SendLoginPin(loginPin.text); loginPin.text = "" } }
                }
            }
            VRow {
                label: qsTr("记住登录 PIN")
                hint: qsTr("加密保存在本机，PS5 要求时自动发送；能登录这台电脑的人都能用它连接")
                VSwitch { checked: veyra.preferences.ps5RememberPin === true; onToggled: checked => veyra.setPreference("ps5RememberPin", checked) }
            }
        }
        DialogGroup {
            visible: ps5Dialog.advanced
            VRow {
                label: qsTr("分辨率与帧率")
                VSelect { implicitWidth: 170; value: ps5Dialog.qualities[ps5Dialog.ps5.quality || 0].label; options: ps5Dialog.qualities; onPicked: id => veyra.ps5Set("quality", Number(id)) }
            }
            VRow {
                label: qsTr("编码")
                hint: qsTr("HDR 需要 PS5 实际输出 HDR 且 Windows HDR 开启")
                VSeg {
                    options: [{ id: "0", label: "H.264" }, { id: "1", label: "H.265" }, { id: "2", label: "H.265 HDR" }]
                    current: String(ps5Dialog.ps5.codec || 0)
                    onPicked: id => veyra.ps5Set("codec", Number(id))
                }
            }
            VRow {
                label: qsTr("请求码率")
                hint: qsTr("发给 PS5 的带宽请求，主机不保证达到")
                VSelect { implicitWidth: 130; value: ps5Dialog.bitrates[ps5Dialog.ps5.bitrate || 0].label; options: ps5Dialog.bitrates; onPicked: id => veyra.ps5Set("bitrate", Number(id)) }
            }
            VRow {
                label: qsTr("解码")
                VSeg {
                    options: [{ id: "0", label: qsTr("自动") }, { id: "1", label: qsTr("CPU 软件") }, { id: "2", label: qsTr("D3D12VA 硬件") }]
                    current: String(ps5Dialog.ps5.decode || 0)
                    onPicked: id => veyra.ps5Set("decode", Number(id))
                }
            }
            VRow {
                label: qsTr("采样")
                hint: qsTr("精细：按位置还原色度，放大用双三次；不是 AI 超分")
                VSeg {
                    options: [{ id: "0", label: qsTr("兼容") }, { id: "1", label: qsTr("精细") }]
                    current: String(ps5Dialog.ps5.sampling === undefined ? 1 : ps5Dialog.ps5.sampling)
                    onPicked: id => veyra.ps5Set("sampling", Number(id))
                }
            }
            VRow {
                visible: veyra.ps5Control.available === true
                label: qsTr("画面用采集卡")
                hint: veyra.ps5Control.enabled === true
                      ? qsTr("已在「采集卡」里打开 PS5 手柄控制：连接采集卡时只用远程游玩传手柄")
                      : qsTr("PS5 的 HDMI 接了采集卡时，可以只用远程游玩传手柄、画面用采集卡原画（可 4K/HDR），也方便再串流给掌机")
                VButton { objectName: "ps5-open-capture"; text: qsTr("采集卡设置"); ghost: true; onClicked: dialogHost.open("capture") }
            }
            VRow {
                label: qsTr("陀螺仪")
                hint: ps5Dialog.ps5.controller ? (ps5Dialog.ps5.gyro ? qsTr("手柄支持陀螺仪") : qsTr("手柄没有陀螺仪")) : qsTr("连接串流及电脑手柄后可校准")
                VButton { text: ps5Dialog.ps5.calibrating ? qsTr("校准中…") : qsTr("校准"); ghost: true; onClicked: veyra.ps5Calibrate() }
            }
        }
    }
    DialogNote { visible: (ps5Dialog.ps5.status || "").length > 0; text: (ps5Dialog.ps5.busy ? qsTr("处理中 · ") : "") + (ps5Dialog.ps5.status || "") }
}
