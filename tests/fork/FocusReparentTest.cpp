// Reproduces the first-stream focus loss: the QML window stops being the active window
// when the native video window is reparented under it. Runs the upstream sequence and
// the fork's sequence and checks which window ends up active.
#include <windows.h>
#include <cstdio>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST && GetParent(h)) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

static HWND makeMain() {
    HWND main = CreateWindowExW(0, L"FocusTestMain", L"main", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetActiveWindow(main);
    return main;
}

static void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

// Upstream v2.1.9: popup without WS_EX_NOACTIVATE, SetParent first, child style after.
static bool upstreamKeepsFocus() {
    HWND main = makeMain();
    HWND video = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW, L"FocusTestVideo", L"",
                                 WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 16, 16,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetParent(video, main);
    const LONG_PTR style = GetWindowLongPtrW(video, GWL_STYLE);
    SetWindowLongPtrW(video, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowPos(video, HWND_TOP, 0, 0, 640, 360, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(video, SW_SHOWNA);
    pump();
    const bool kept = GetActiveWindow() == main;
    std::printf("upstream: active=%s\n", GetActiveWindow() == main ? "main" : GetActiveWindow() == video ? "video" : "other");
    DestroyWindow(video); DestroyWindow(main); pump();
    return kept;
}

// Fork: WS_EX_NOACTIVATE, child style set before SetParent.
static bool forkKeepsFocus() {
    HWND main = makeMain();
    HWND video = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"FocusTestVideo", L"",
                                 WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 16, 16,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const LONG_PTR style = GetWindowLongPtrW(video, GWL_STYLE);
    SetWindowLongPtrW(video, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetParent(video, main);
    SetWindowPos(video, HWND_TOP, 0, 0, 640, 360, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(video, SW_SHOWNA);
    pump();
    const bool kept = GetActiveWindow() == main;
    const bool child = (GetWindowLongPtrW(video, GWL_STYLE) & WS_CHILD) != 0 && GetParent(video) == main;
    std::printf("fork:     active=%s child=%d\n", GetActiveWindow() == main ? "main" : GetActiveWindow() == video ? "video" : "other", child);
    DestroyWindow(video); DestroyWindow(main); pump();
    return kept && child;
}

int main() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FocusTestMain";
    RegisterClassExW(&wc);
    wc.lpszClassName = L"FocusTestVideo";
    RegisterClassExW(&wc);
    const bool upstream = upstreamKeepsFocus();
    const bool fork = forkKeepsFocus();
    std::printf("upstream keeps focus: %s\nfork keeps focus: %s\n", upstream ? "yes" : "NO", fork ? "yes" : "NO");
    return fork ? 0 : 1;
}
