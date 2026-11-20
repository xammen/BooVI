// The mockups' ordered-dither backdrop: a 4x4 Bayer threshold over a slow wave, drawn at
// one canvas pixel per `cell` screen pixels and scaled up unsmoothed so the dots stay crisp.
// mode "horizon" is the home page's hill with twinkling stars above it, "band" the soft
// strip behind a page header, "stage" the stream preview's horizon and ridge.
// It ticks at 20 Hz only while visible; reduced motion paints one still frame. A page
// switch holds it still (Main.qml `navigating`): its repaints on the interface thread
// dropped frames of the switch, with gaps of ~50 ms between presents instead of ~17.
import QtQuick

Item {
    id: root
    property string mode: "horizon"
    property real cell: 3
    property bool animate: true
    readonly property int cols: Math.max(1, Math.round(width / cell))
    readonly property int rows: Math.max(1, Math.round(height / cell))
    property real t: 0
    readonly property bool held: Window.window !== null && Window.window.navigating === true
    property bool painted: false
    clip: true

    Canvas {
        id: canvas
        width: root.cols
        height: root.rows
        scale: root.cell
        transformOrigin: Item.TopLeft
        smooth: false
        renderTarget: Canvas.Image
        onPaint: { root.paintInto(getContext("2d")); root.painted = true }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
    onVisibleChanged: if (visible && (!held || !painted)) canvas.requestPaint()
    Timer {
        interval: 50
        repeat: true
        running: root.visible && root.animate && !Theme.reduced && root.width > 0 && !root.held
        onTriggered: { root.t += 0.025; canvas.requestPaint() }
    }
    Connections { target: Theme; function onPaletteNameChanged() { canvas.requestPaint() } }

    // Dots are bucketed into eight alpha levels and drawn as 1x1 rects, one fillStyle per
    // level: putImageData does not reach the image on this Qt build (read back as empty).
    function paintInto(ctx) {
        const cols = root.cols, rows = root.rows
        ctx.clearRect(0, 0, cols, rows)
        if (cols < 2 || rows < 2) return
        const B = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]
        const levels = [[], [], [], [], [], [], [], []]
        const t = root.t
        for (let x = 0; x < cols; ++x) {
            const p = x / (cols - 1)
            let top = 0, ridge = rows, wave = 0
            if (root.mode === "horizon")
                top = (0.5 - Math.sin(p * Math.PI) * 0.22 + Math.sin(p * 9.2 + t) * 0.014 + Math.sin(t * 0.4) * 0.006) * rows
            else if (root.mode === "stage") {
                top = (0.68 - Math.sin(p * Math.PI) * 0.12 + Math.sin(p * 6 + t) * 0.01) * rows
                ridge = (0.6 + Math.sin(p * 13 + 1.7) * 0.05 + Math.sin(p * 31) * 0.015) * rows
            } else
                wave = 0.55 + 0.45 * Math.sin(x / cols * 5 + t)
            for (let y = 0; y < rows; ++y) {
                const th = (B[((y & 3) << 2) | (x & 3)] + 0.5) / 16
                let a = 0
                if (root.mode === "band") {
                    if (Math.pow(1 - y / rows, 1.6) * 0.5 * wave > th) a = 0.43
                } else if (y >= top) {
                    const depth = (y - top) / Math.max(rows - top, 1)
                    if (root.mode === "horizon") {
                        const edge = Math.exp(-Math.pow((y - top - rows * 0.06) / (rows * 0.05), 2))
                        const dens = Math.min(1, edge * 0.95 + (1 - depth) * 0.38 + 0.06)
                        a = dens > th ? 0.32 + 0.6 * edge : (dens * 0.45 > th ? 0.14 : 0)
                    } else {
                        const edge = Math.exp(-Math.pow((y - top - rows * 0.02) / (rows * 0.025), 2))
                        const dens = Math.min(1, edge + (1 - depth) * 0.3 + 0.05)
                        a = dens > th ? 0.25 + 0.6 * edge : 0
                    }
                } else if (y >= ridge) {
                    a = 0.2 > th ? 0.18 : 0
                } else {
                    if (root.mode === "stage" && (y / rows) * 0.22 > th) a = 0.1
                    let h = Math.imul(x, 374761393) ^ Math.imul(y, 668265263)
                    h = (h ^ (h >>> 13)) >>> 0
                    if (h % 700 === 0) { const tw = 0.5 + 0.5 * Math.sin(t * 2.4 + h); if (tw > 0.55) a = tw * 0.9 }
                }
                if (a > 0) levels[Math.min(7, Math.floor(Math.min(1, a) * 8))].push(x, y)
            }
        }
        const ink = Theme.ditherInk
        for (let l = 0; l < 8; ++l) {
            const dots = levels[l]
            if (dots.length === 0) continue
            ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, ((l + 0.5) / 8) * 0.9)
            for (let i = 0; i < dots.length; i += 2) ctx.fillRect(dots[i], dots[i + 1], 1, 1)
        }
    }
}
