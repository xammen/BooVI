// The three ways to play, mapped onto the engine's own stages (no new engine behaviour):
//   performance  native picture, no super resolution, no frame generation (lowest latency)
//   balanced     super resolution to the display's size, no frame generation
//   fluid        super resolution plus frame generation at the multiplier chosen in réglages,
//                with the output cap following the display
// The current preset is read back from the chain, so a change made elsewhere shows here;
// SR off with FG on matches none of them and leaves the control unselected.
import QtQuick

QtObject {
    id: p
    readonly property bool sr: veyra.srEnabled
    readonly property bool fg: veyra.fgEnabled
    readonly property string current: !sr && !fg ? "performance" : sr && !fg ? "balanced" : sr && fg ? "fluid" : ""
    readonly property var options: [
        { id: "performance", label: qsTr("performance") },
        { id: "balanced", label: qsTr("équilibré") },
        { id: "fluid", label: qsTr("fluidité") }
    ]
    // Multipliers the chosen frame generation backend offers, as [{ id: "2", label: "x2" }].
    readonly property var multipliers: (veyra.fgMultiplierChoices || []).map(o => ({ id: o.id, label: "x" + o.id }))
    readonly property int fluidMultiplier: {
        const want = Number(veyra.preferences.fluidMultiplier || 2)
        const ids = multipliers.map(o => Number(o.id))
        if (ids.indexOf(want) >= 0) return want
        return ids.length > 0 ? ids[0] : 2
    }

    // SR targets the engine knows (srTargetIndex ids) by the height they reach.
    function targetForHeight(px) {
        if (px > 3600) return 3
        if (px > 2880) return 6
        if (px > 2160) return 4
        if (px > 1440) return 2
        return 1
    }
    function targetLabel(index) {
        return ({ 1: "1440p", 2: "4k", 3: "8k", 4: "5k", 5: "6k", 6: "7k" })[index] || qsTr("natif")
    }
    function hasStage(type) {
        const chain = veyra.chain || []
        for (let i = 0; i < chain.length; ++i) if (chain[i].type === type) return true
        return false
    }
    function ensureStage(type) { if (!hasStage(type)) veyra.addEffect(type) }

    // screenHeightPx: the physical height of the display the window is on.
    function apply(id, screenHeightPx) {
        veyra.logUi("ui-preset", "apply " + id)
        if (id === "fgonly") {
            // Test switch only (scripts/fork/fgbench): frame generation on the source picture.
            veyra.srEnabled = false
            ensureStage("frame-generation")
            if (veyra.outputRateMode !== 1) veyra.outputRateMode = 1
            veyra.fgEnabled = true
            veyra.fgMultiplier = fluidMultiplier
            return
        }
        if (id === "performance") {
            veyra.fgEnabled = false
            veyra.srEnabled = false
            return
        }
        ensureStage("sr")
        veyra.srTargetIndex = targetForHeight(screenHeightPx)
        veyra.srEnabled = true
        if (id === "balanced") { veyra.fgEnabled = false; return }
        ensureStage("frame-generation")
        if (veyra.outputRateMode !== 1) veyra.outputRateMode = 1
        veyra.fgMultiplier = fluidMultiplier
    }

    // What the screen gets: the engine's measured target while it runs, otherwise the source
    // rate times the multiplier, capped by the display when the cap follows it.
    function outputFps(sourceFps) {
        if (veyra.effectiveOutputFps > 0) return veyra.effectiveOutputFps
        const base = sourceFps > 0 ? sourceFps : 60
        const m = fg ? Math.max(1, veyra.fgMultiplier) : 1
        const hz = veyra.displayRefreshHz
        return veyra.outputRateMode === 1 && hz > 1 ? Math.min(base * m, hz) : base * m
    }
    function summary(id) {
        if (id === "performance") return qsTr("natif")
        const target = targetLabel(veyra.srTargetIndex)
        return id === "balanced" ? qsTr("sr → %1").arg(target) : qsTr("sr → %1 · fg x%2").arg(target).arg(fg ? veyra.fgMultiplier : fluidMultiplier)
    }
}
