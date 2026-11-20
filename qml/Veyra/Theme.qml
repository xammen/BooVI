// Design tokens, transcribed from the approved prototype's app.css.
//
// Every classic value here is copied from the prototype rather than chosen. That
// is the point of this file: the interface has to match the approved design, so
// the numbers live in one place that can be diffed against app.css.
//
// 设置 → 外观 → 界面配色 adds a second palette, 日落 (dusk, the default since
// 2026-11-14): the same depths tinted indigo-violet, a sunset accent that fills
// switches, sliders, primary buttons and seek bars as a peach -> pink gradient
// (VAccentFill.qml), and a twilight backdrop (tools/qt_probe/make-backdrop-dusk.py).
// Its values are our own, chosen after sunset skies. "classic" keeps every value
// of the prototype exactly.
// Fork: a third palette, sage (the default), from BooVI's own art direction:
// violet-neutral darks, one scarce violet accent (#9B7BEA) for state and focus, flat solid
// fills, JetBrains Mono for controls. It keeps the classic structure (no sunset gradient,
// backdrop or intro) and overrides the tokens below.
pragma Singleton
import QtQuick

QtObject {
    // --- palette ------------------------------------------------------------
    property string paletteName: "sage"
    readonly property bool sage: paletteName === "sage"
    readonly property bool dusk: paletteName === "dusk"

    // --- surfaces (app.css .vy) -------------------------------------------
    readonly property color card: sage ? "#17141D" : dusk ? "#161425" : "#131317"
    readonly property color card2: sage ? "#1B1822" : dusk ? "#1C192C" : "#19191E"
    readonly property color card3: sage ? "#201C27" : dusk ? "#262238" : "#24242A"
    readonly property color popover: sage ? "#1B1822" : dusk ? "#1D1A2D" : "#1B1B20"
    readonly property color dialog: sage ? "#17141D" : dusk ? "#171526" : "#141418"
    readonly property color stroke: sage ? Qt.rgba(1, 1, 1, 0.09) : dusk ? Qt.rgba(0.88, 0.84, 1, 0.09) : Qt.rgba(1, 1, 1, 0.07)
    readonly property color stroke2: sage ? Qt.rgba(1, 1, 1, 0.14) : dusk ? Qt.rgba(0.88, 0.84, 1, 0.16) : Qt.rgba(1, 1, 1, 0.13)
    // The app background: a radial gradient in the prototype, approximated here
    // with the three stops it interpolates between.
    readonly property color bgOuter: sage ? "#0F0D14" : dusk ? "#100E1D" : "#070709"
    readonly property color bgMid: sage ? "#0F0D14" : dusk ? "#161328" : "#0C0C10"
    readonly property color bgInner: sage ? "#13111A" : dusk ? "#201B34" : "#17171C"
    readonly property color videoBlack: "#000000"
    // Dark neutrals some components still spell out (the cinema pill, the node
    // canvas, the dock): exactly that colour in the classic palette, the same
    // depth tinted indigo-violet in the dusk one.
    function shade(r, g, b, a) {
        const alpha = a === undefined ? 1 : a
        return dusk ? Qt.rgba(Math.min(255, r + 2) / 255, Math.max(0, g - 1) / 255, Math.min(255, b + 14) / 255, alpha)
                    : Qt.rgba(r / 255, g / 255, b / 255, alpha)
    }

    // --- text -------------------------------------------------------------
    readonly property color t1: sage ? "#EBE8F0" : dusk ? "#F7F4FC" : "#F3F3F5"
    readonly property color t2: sage ? "#A29EAB" : dusk ? Qt.rgba(0.969, 0.957, 0.988, 0.64) : Qt.rgba(0.953, 0.953, 0.961, 0.62)
    readonly property color t3: sage ? "#8D8996" : dusk ? Qt.rgba(0.969, 0.957, 0.988, 0.40) : Qt.rgba(0.953, 0.953, 0.961, 0.38)

    // --- accent and state -------------------------------------------------
    // 设置 → 强调色: orange (default; the sunset accent in the dusk palette) or
    // the design's .accent-white variant (a neon white with a pink glow in dusk).
    property string accentName: "orange"
    readonly property bool accentWhite: accentName === "white"
    // Sage keeps one accent whatever the accent setting says: the violet is the identity.
    readonly property color accent: sage ? "#9B7BEA" : accentWhite ? (dusk ? "#FFF6FB" : "#F3F3F5") : (dusk ? "#F287A7" : "#FF8A3D")
    readonly property color accentInk: sage ? "#0F0D14" : accentWhite ? (dusk ? "#1E0B1A" : "#0B0B0E") : (dusk ? "#2A0B1E" : "#1B0E04")
    readonly property color accentSoft: sage ? Qt.rgba(0.608, 0.482, 0.918, 0.16) : accentWhite ? Qt.rgba(1, 1, 1, 0.1)
                                      : dusk ? Qt.rgba(0.949, 0.529, 0.655, 0.15) : Qt.rgba(1, 0.541, 0.239, 0.14)
    readonly property color accentGlow: sage ? Qt.rgba(0.608, 0.482, 0.918, 0.30) : accentWhite ? (dusk ? Qt.rgba(0.96, 0.62, 0.82, 0.42) : Qt.rgba(1, 1, 1, 0.35))
                                      : dusk ? Qt.rgba(0.949, 0.529, 0.655, 0.38) : Qt.rgba(1, 0.541, 0.239, 0.35)
    // The sunset accent fills as a gradient (VAccentFill.qml): peach -> pink,
    // on to lilac along the seek bars.
    readonly property bool accentGradient: dusk && !accentWhite
    readonly property color accentFrom: "#F0B17B"
    readonly property color accentTo: "#EF6FA3"
    readonly property color accentFar: "#BB96F3"
    // 设置 → 背景渐变强度: 0 none, 1 light (the design default), 2 medium.
    property int backdropLevel: 1
    readonly property string backdropImage: (dusk ? "backdrop-dusk" : "backdrop") + (backdropLevel === 2 ? "-strong" : "") + ".png"
    // The logo: BooVI's mark (assets/boovi.png, scripts/fork/make-icon.py) in every palette.
    readonly property string logoImage: dusk ? "logo-dusk.png" : "logo.png"
    // Fork: "ok" is a bright neutral ink, not a green (La Cuisine's --success). A good state reads
    // as a filled dot or plain words; colour stays for warnings and errors only.
    readonly property color ok: sage ? "#D9D6DE" : dusk ? "#E2DDEE" : "#D9D9D5"
    readonly property color okGlow: sage ? Qt.rgba(0.373, 0.749, 0.514, 0.16) : dusk ? Qt.rgba(0.369, 0.831, 0.698, 0.16) : Qt.rgba(0.239, 0.863, 0.518, 0.16)
    readonly property color warn: "#F2C572"
    readonly property color warnGlow: Qt.rgba(0.961, 0.784, 0.294, 0.16)
    readonly property color err: "#EEA27F"
    readonly property color errGlow: Qt.rgba(1, 0.365, 0.365, 0.16)
    readonly property color exp: sage ? "#B9A2F4" : "#B79BFF"
    readonly property color expSoft: Qt.rgba(0.718, 0.608, 1.0, 0.14)

    // --- geometry (--r-win / --r-card / --r-ctl) --------------------------
    // The window radius is 8px, small, because Windows' own default is small.
    // The video surface is NEVER rounded: .video in the prototype carries
    // border-radius:0!important because rounding crops the picture.
    readonly property int rWindow: 8
    readonly property int rCard: sage ? 8 : 14
    readonly property int rCtl: sage ? 4 : 10

    // --- fork shell (the approved 2026-11 mockups) -------------------------------
    // Radii nest: a control (4) inside a 2px-padded segmented track gets 2, a card
    // with 4px of padding around controls gets 8. Divider lines are structure, rings
    // (a 1px border at low alpha) give surfaces their edge, depth comes from shadows.
    readonly property int rSm: 2
    readonly property int rMd: 4
    readonly property int rLg: 8
    readonly property color line: sage ? "#29252F" : dusk ? "#2A2641" : "#232329"
    readonly property color line2: sage ? "#3B3644" : dusk ? "#3A3454" : "#34343C"
    readonly property color ringStrong: Qt.rgba(1, 1, 1, 0.12)
    readonly property color bgStage: sage ? "#07060A" : dusk ? "#08070F" : "#050506"
    readonly property color fieldBg: bgOuter
    readonly property color hoverBg: sage ? "#201C27" : dusk ? "#262238" : "#24242A"
    readonly property color rowHover: sage ? "#15121A" : dusk ? "#18152A" : "#111114"
    // The solid "play" button: near-white on the dark shell, white on hover.
    readonly property color solid: t1
    readonly property color solidHover: "#FFFFFF"
    readonly property color solidInk: bgOuter
    // Text on a selected segment, and the brighter accent used for numbers.
    readonly property color accentHi: sage ? "#DDD2FB" : dusk ? "#FFD9E6" : accentWhite ? "#FFFFFF" : "#FFD2B0"
    readonly property color accentText: sage ? "#B9A2F4" : dusk ? "#F5A3C0" : accentWhite ? "#F3F3F5" : "#FFAA70"
    readonly property color accentRing: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    readonly property color accentFill: Qt.rgba(accent.r, accent.g, accent.b, 0.20)
    // The dither backdrop's ink (UiDither.qml): lilac, sunset pink, or a neutral grey.
    readonly property color ditherInk: sage ? "#C4B6FF" : dusk ? "#F5A8C8" : "#D6D6DE"
    readonly property real fsLabel: 10
    readonly property real fsMeta: 11
    readonly property real fsCtl: 12
    readonly property real fsText: 13
    readonly property real fsRow: 14
    readonly property real fsLead: 15
    readonly property real fsTitle: 32
    readonly property real fsHero: 48
    // Fork motion, from La Cuisine (docs/UI_POLISH_2026-11-20.md).
    // cubic-bezier(0.22, 1, 0.36, 1): states, entrances and glides; fast start, long soft landing.
    readonly property var ease: [0.22, 1, 0.36, 1, 1, 1]
    // cubic-bezier(0.4, 0, 1, 1): exits, which accelerate away and end sooner than entrances.
    readonly property var easeExit: [0.4, 0, 1, 1, 1, 1]
    readonly property int durHover: 150     // colour of hover and press, nothing else
    readonly property int durState: 240     // a switch flips, a check appears
    readonly property int durGlide: 380     // a selection slides to its new place
    readonly property int durEnter: 420     // a dialog or a block arrives
    readonly property int durExit: 180      // and leaves
    readonly property real pressScale: 0.96
    // Selections are lit grey, as in La Cuisine: the violet accent only marks keyboard and
    // controller focus and the dot of a radio. knobHi/knobLo is a raised selected surface, lit
    // its 1px top highlight, shadow the 1px drop under anything raised.
    readonly property color selBg: Qt.rgba(1, 1, 1, 0.06)
    readonly property color selLine: Qt.rgba(1, 1, 1, 0.12)
    readonly property color knobHi: sage ? "#2C2833" : dusk ? "#2E2944" : "#2C2C30"
    readonly property color knobLo: sage ? "#24212A" : dusk ? "#25213A" : "#242428"
    readonly property color lit: Qt.rgba(1, 1, 1, 0.07)
    readonly property color shadow: Qt.rgba(0, 0, 0, 0.45)
    readonly property color focusRing: Qt.rgba(accent.r, accent.g, accent.b, 0.8)

    // --- motion (--spring / --spring-soft / --out) ------------------------
    // easing.bezierCurve takes whole cubic segments, six numbers each
    // [c1x, c1y, c2x, c2y, endX, endY], the last one ending at (1, 1). Any other
    // length is rejected and the animation silently runs linear (G0.5/G0.6 caught
    // the old four-number curves doing exactly that).
    // --spring and --spring-soft are CSS linear() functions: straight lines between
    // stops. A cubic with its control points at 1/3 and 2/3 of a line is that line,
    // so each stop becomes one segment and the curve matches the design exactly,
    // overshoot included. Stops without a position are spread evenly, as in CSS.
    // At most 10 segments: Qt 6.8.3 corrupts the heap when a BezierSpline curve of
    // 11+ segments is destroyed (reproduced in C++ with no QML, G1.1). --spring has
    // 12, so its 1.035@28.4% and .998@38.5% stops are dropped; the remaining line
    // stays within 0.007 of the design curve (veyra_qml_easing_tests checks 0.01).
    // Reduced motion (the design's .vy.reduced: every transition and animation at
    // 0s). Main.qml binds it to the saved setting or the --reduced-motion switch;
    // every duration in the app goes through d(), so one flag covers all of them.
    // Endless animations also stop on it: a zero-length loop would spin.
    property bool reduced: false
    function d(ms) { return reduced ? 0 : ms }

    readonly property int durFast: 150
    readonly property int durNormal: 240
    readonly property int durSlow: 450
    function linearEasing(stops) {
        var out = []
        for (var i = 1; i < stops.length; ++i) {
            var a = stops[i - 1], b = stops[i]
            out.push(a[0] + (b[0] - a[0]) / 3, a[1] + (b[1] - a[1]) / 3,
                     a[0] + (b[0] - a[0]) * 2 / 3, a[1] + (b[1] - a[1]) * 2 / 3,
                     b[0], b[1])
        }
        return out
    }
    // linear(0,.009,.035 2.1%,.141 4.4%,.723 12.9%,.938 16.7%,1.017 20.2%,1.043 24%,
    //        1.035 28.4%,.998 38.5%,.99 44.1%,1.001 60.7%,1)
    readonly property var spring: linearEasing([
        [0, 0], [0.0105, 0.009], [0.021, 0.035], [0.044, 0.141], [0.129, 0.723],
        [0.167, 0.938], [0.202, 1.017], [0.24, 1.043],
        [0.441, 0.99], [0.607, 1.001], [1, 1]])
    // linear(0,.02,.08 3.2%,.3 7.5%,.84 17%,1.01 25%,1.025 31%,1.004 45%,1)
    readonly property var springSoft: linearEasing([
        [0, 0], [0.016, 0.02], [0.032, 0.08], [0.075, 0.3], [0.17, 0.84],
        [0.25, 1.01], [0.31, 1.025], [0.45, 1.004], [1, 1]])
    // cubic-bezier(.2,.8,.2,1)
    readonly property var easeOut: [0.2, 0.8, 0.2, 1.0, 1.0, 1.0]

    // --- type (--f-ui / --f-mono and the .h1/.h2/.h3 scale) ---------------
    // --f-ui is "Geist", "Noto Sans SC", ...: Geist is not on this machine (D4), so
    // the design's own second choice, which is also what Edge rendered the reference
    // with. Noto Sans SC ships as a variable font; Qt 6.8.3 maps font.weight 600 to a
    // thin instance of it (500 and 700 are fine), so every DemiBold also sets the wght
    // axis directly (G1.3, tmp/.../min/Font.qml).
    // Per interface language (Qt.uiLanguage, set by the bridge): Japanese kana and kanji
    // and Traditional characters read best in their own Windows UI fonts.
    // Fork: static Segoe UI, the mockups' fallback. Qt's software rasteriser (the default
    // interface renderer) gives Segoe UI Variable's named instances a wrong advance for "c"
    // at 13.5-14px, so "clavier" drew as "davier".
    readonly property string fontUi: Qt.uiLanguage === "ja" ? "Yu Gothic UI"
                                   : Qt.uiLanguage === "zh-TW" ? "Microsoft JhengHei UI"
                                   : sage && (Qt.uiLanguage === "en" || Qt.uiLanguage === "fr") ? "Segoe UI" : "Noto Sans SC"
    readonly property var axesDemiBold: ({ "wght": 600 })
    // 500: the design's .seek .peek time strip is `font: 500 11px/1 var(--f-mono)`.
    // Same reason as DemiBold - 600 lands on a thin instance, so the axis is set
    // explicitly rather than left to font.weight.
    readonly property var axesMedium: ({ "wght": 500 })
    // JetBrains Mono ships in fonts/ (SIL OFL 1.1, fonts/JetBrainsMono-OFL.txt).
    property FontLoader monoRegular: FontLoader { source: "fonts/JetBrainsMono-Regular.ttf" }
    property FontLoader monoMedium: FontLoader { source: "fonts/JetBrainsMono-Medium.ttf" }
    readonly property string fontMono: sage && monoRegular.status === FontLoader.Ready ? monoRegular.font.family : "Consolas"
    readonly property real fsEyebrow: 11
    readonly property real fsSmall: 11.5
    readonly property real fsBody: 12.5
    readonly property real fsH3: 15
    readonly property real fsH2: 18
    readonly property real fsH1: 26

    // --- component metrics from the CSS -----------------------------------
    readonly property int ctlHeight: 32      // .btn / .pill
    readonly property int rowMinHeight: 38   // .row
    readonly property int switchWidth: 34    // .sw
    readonly property int switchHeight: 20
    readonly property int switchKnob: 14
    readonly property int sliderKnob: 13
    readonly property int tagHeight: 20
    readonly property int dialogWidth: 640
    readonly property int popoverMinWidth: 220
}
