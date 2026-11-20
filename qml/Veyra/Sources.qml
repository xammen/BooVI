// The four sources the fork plays, as one table: what each is called, whether it is set
// up, what is live, and how to start or end it. Pages read rows from here instead of
// testing each source's state shape themselves.
import QtQuick

QtObject {
    id: s
    readonly property var ps5: veyra.ps5 || ({})
    readonly property var pc: veyra.moonlight ? veyra.moonlight.state : ({})
    readonly property var xbox: veyra.xbox ? veyra.xbox.state : ({})
    // A friend's share joined from this BooVI (GuestJoinModel.h).
    readonly property var join: veyra.join ? veyra.join.state : ({})
    readonly property var ids: veyra.join ? ["ps5", "xbox", "pc", "capture", "join"] : ["ps5", "xbox", "pc", "capture"]
    // The kind of the stream on screen, "" when none.
    readonly property string live: ps5.active === true ? "ps5"
                                 : pc.streaming === true ? "pc"
                                 : xbox.streaming === true ? "xbox"
                                 : join.streaming === true ? "join"
                                 : veyra.hasSource && veyra.isCapture ? "capture" : ""
    readonly property string lastKind: ({ ps5: "ps5", moonlight: "pc", xbox: "xbox", capture: "capture", join: "join" })[veyra.lastSource.kind || ""] || ""

    function title(id) {
        switch (id) {
        case "ps5": return "ps5"
        case "pc": return (pc.hostName || pc.lastLabel || qsTr("pc")).toLowerCase()
        case "xbox": return (xbox.lastLabel || "xbox").toLowerCase()
        case "join": return qsTr("rejoindre un partage")
        default: return veyra.hasCaptureSession ? veyra.captureSessionSummary.split(" · ")[0].toLowerCase() : qsTr("carte de capture")
        }
    }
    function kindLabel(id) {
        return ({ ps5: "PS5 REMOTE PLAY", pc: qsTr("STREAMING PC"), xbox: qsTr("STREAMING XBOX"), capture: qsTr("CARTE DE CAPTURE"), join: qsTr("PARTAGE BOOVI") })[id]
    }
    function shortName(id) { return ({ ps5: "ps5", pc: "pc", xbox: "xbox", capture: qsTr("capture"), join: qsTr("partage") })[id] }
    function icon(id) { return ({ ps5: "pad", pc: "pc", xbox: "pad", capture: "capturecard", join: "users" })[id] }
    function configured(id) {
        switch (id) {
        case "ps5": return (veyra.remotePlayHost || "").length > 0 || (veyra.ps5Profiles || []).length > 0
        case "pc": return (pc.lastLabel || "").length > 0
        case "xbox": return (xbox.lastLabel || "").length > 0
        case "join": return true
        default: return veyra.hasCaptureSession === true
        }
    }
    // One honest line under the name: what is known about it, never a guess.
    function detail(id) {
        if (live === id) return qsTr("en jeu")
        switch (id) {
        case "ps5": return [qsTr("associée"), veyra.remotePlayHost || ps5.host || ""].filter(x => x.length).join(" · ")
        case "pc": return pc.online === true ? qsTr("en ligne") : qsTr("associé")
        case "xbox": return xbox.signedIn === true ? qsTr("compte connecté") : qsTr("à reconnecter")
        case "join": return (join.saved || "").length > 0 && join.access === true ? join.saved
                          : join.linkValid === true ? qsTr("hôte %1").arg(join.host || "") : qsTr("colle le lien de ton ami")
        default: { const parts = (veyra.captureSessionSummary || "").split(" · "); parts.shift(); return parts.join(" · ").toLowerCase() || qsTr("dernier réglage gardé") }
        }
    }
    function state(id) { return live === id ? "live" : id === "pc" && pc.online === true ? "online" : "idle" }
    readonly property var rows: {
        const order = ids.slice().sort((a, b) => (b === live) - (a === live) || (b === lastKind) - (a === lastKind))
        return order.filter(id => configured(id))
    }

    // Starts the source's last session. Returns false when it needs its setup page instead.
    function play(id) {
        veyra.logUi("ui-source", "play " + id)
        switch (id) {
        case "ps5": veyra.ps5Load(); veyra.ps5Play(); return true
        case "pc": return !!veyra.moonlight && veyra.moonlight.resumeLast()
        case "xbox": return !!veyra.xbox && veyra.xbox.resumeLast()
        case "join": return !!veyra.join && veyra.join.resumeLast()
        default: veyra.resumeCaptureSession(); return true
        }
    }
    function quit(id) {
        veyra.logUi("ui-source", "quit " + id)
        switch (id) {
        case "ps5": veyra.ps5Cancel(); break
        case "pc": veyra.moonlightDisconnect(); break
        case "xbox": veyra.xboxDisconnect(); break
        case "join": veyra.stopPlayback(); break
        default: veyra.stopPlayback()
        }
    }
}
