// Fork shell icons (Lucide, ISC license, lucide.dev; see THIRD_PARTY_NOTICES.md), the ones the
// 2026-11 mockups use and IconData.js (generated, not edited by hand) does not have.
.pragma library

var paths = {
    "pad": "M6 11h4M8 9v4M15 12h.01M18 10h.01M17.32 5H6.68a4 4 0 0 0 -3.978 3.59C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5 -.5 2 -1l1.414 -1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414 .586L17 18c.5 .5 1 1 2 1a3 3 0 0 0 3 -3c0 -1.545 -.604 -6.584 -.685 -7.258A4 4 0 0 0 17.32 5z",
    "keyboard": "M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-16a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2zM6 8h.01M10 8h.01M14 8h.01M18 8h.01M8 12h.01M12 12h.01M16 12h.01M7 16h10",
    "tune": "M21 4h-7M10 4h-7M21 12h-9M8 12h-5M21 20h-5M12 20h-9M14 2v4M8 10v4M16 18v4",
    "arrowleft": "M12 19l-7 -7l7 -7M19 12h-14",
    "history": "M3 12a9 9 0 1 0 9 -9a9.75 9.75 0 0 0 -6.74 2.74L3 8M3 3v5h5M12 7v5l4 2",
    "expand": "M15 3h6v6M9 21h-6v-6M21 3l-7 7M3 21l7 -7",
    "shrink": "M4 14h6v6M20 10h-6v-6M14 10l7 -7M3 21l7 -7",
    "winmin": "M5 12h14",
    "winmax": "M6 4h12a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2z",
    "winclose": "M18 6l-12 12M6 6l12 12",
    "pausebars": "M15 4h3a1 1 0 0 1 1 1v14a1 1 0 0 1 -1 1h-3a1 1 0 0 1 -1 -1v-14a1 1 0 0 1 1 -1zM6 4h3a1 1 0 0 1 1 1v14a1 1 0 0 1 -1 1h-3a1 1 0 0 1 -1 -1v-14a1 1 0 0 1 1 -1z",
    "capturecard": "M3 7h18a1 1 0 0 1 1 1v8a1 1 0 0 1 -1 1h-18a1 1 0 0 1 -1 -1v-8a1 1 0 0 1 1 -1zM6 11h4M18 12h.01M15 12h.01",
    "pc": "M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1 -2 2h-16a2 2 0 0 1 -2 -2v-10a2 2 0 0 1 2 -2zM8 21h8M12 17v4",
    // Partager: the stream toolbar's share button and the kick action (Lucide users, user-x).
    "users": "M16 21v-2a4 4 0 0 0 -4 -4h-6a4 4 0 0 0 -4 4v2M9 3a4 4 0 1 0 0 8a4 4 0 1 0 0 -8zM22 21v-2a4 4 0 0 0 -3 -3.87M16 3.13a4 4 0 0 1 0 7.75",
    "userx": "M16 21v-2a4 4 0 0 0 -4 -4h-6a4 4 0 0 0 -4 4v2M9 3a4 4 0 1 0 0 8a4 4 0 1 0 0 -8zM17 8l5 5M22 8l-5 5",
    "volx": "M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298zM22 9l-6 6M16 9l6 6",

    // DualSense controls for the bindings card (PadGlyph.qml), drawn for this app, not Lucide.
    "ps-triangle": "M12 4.6l7.6 13.2h-15.2z",
    "ps-circle": "M12 4.5a7.5 7.5 0 1 0 0 15a7.5 7.5 0 1 0 0 -15z",
    "ps-cross": "M5.6 5.6l12.8 12.8M18.4 5.6l-12.8 12.8",
    "ps-square": "M5.5 5.5h13v13h-13z",
    "ps-stick": "M12 2.5a9.5 9.5 0 1 0 0 19a9.5 9.5 0 1 0 0 -19z",
    "ps-stick-cap": "M12 7.5a4.5 4.5 0 1 0 0 9a4.5 4.5 0 1 0 0 -9z",
    "ps-chevron-up": "M8 14l4 -4l4 4",
    "ps-chevron-down": "M8 10l4 4l4 -4",
    "ps-chevron-left": "M14 8l-4 4l4 4",
    "ps-chevron-right": "M10 8l4 4l-4 4",
    "ps-dpad": "M9.5 2.5h5v5.5l-2.5 2.5l-2.5 -2.5zM9.5 21.5h5v-5.5l-2.5 -2.5l-2.5 2.5zM2.5 9.5v5h5.5l2.5 -2.5l-2.5 -2.5zM21.5 9.5v5h-5.5l-2.5 -2.5l2.5 -2.5z",
    "ps-dpad-up": "M9.5 2.5h5v5.5l-2.5 2.5l-2.5 -2.5z",
    "ps-dpad-down": "M9.5 21.5h5v-5.5l-2.5 -2.5l-2.5 2.5z",
    "ps-dpad-left": "M2.5 9.5v5h5.5l2.5 -2.5l-2.5 -2.5z",
    "ps-dpad-right": "M21.5 9.5v5h-5.5l-2.5 -2.5l2.5 -2.5z",
    "ps-options": "M7 8h10M7 12h10M7 16h10",
    "ps-create": "M12 10v-6M8.2 11.5l-3.6 -3.6M15.8 11.5l3.6 -3.6M7 17h10",
    "ps-touchpad": "M4.5 7h15a2 2 0 0 1 2 2v4.5a4 4 0 0 1 -4 4h-11a4 4 0 0 1 -4 -4v-4.5a2 2 0 0 1 2 -2zM12 7v10.5"
}
