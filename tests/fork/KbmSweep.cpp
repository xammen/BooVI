// Live probe for the PS5 keyboard/mouse latency: injects relative mouse movements (SendInput,
// which raises WM_INPUT like a real mouse) into one running Veyra window and logs when each was
// sent, on the clock the app's VEYRA_KBM_TRACE uses (steady_clock, 100 ns). Every sweep moves
// at a constant speed for --ms, then rests until the next --period; directions alternate so the
// camera comes back. It injects only while that process's window is in front, and stops otherwise.
// Usage: KbmSweep.exe <pid> <out.csv> [--wait 1000 (after the capture starts)] [--ms 200] [--period 1000]
//        [--speeds 250,500,1000,2000,4000] [--rates 1000,125]
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
long long now100ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() / 100; }

struct Find { DWORD pid; HWND best; long area; };
BOOL CALLBACK pick(HWND h, LPARAM p) {
    auto& f = *reinterpret_cast<Find*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r{};
    if (pid != f.pid || !IsWindowVisible(h) || !GetWindowRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > f.area) { f.area = area; f.best = h; }
    return TRUE;
}

bool ours(DWORD pid) {
    DWORD fg = 0;
    const HWND h = GetForegroundWindow();
    return h && GetWindowThreadProcessId(h, &fg) && fg == pid;
}

// The app clips the cursor to its window while it captures the keyboard and mouse. Without
// that clip a relative move would drive the desktop cursor, so nothing is injected then.
bool captured() {
    RECT clip{};
    GetClipCursor(&clip);
    return clip.right - clip.left < GetSystemMetrics(SM_CXVIRTUALSCREEN) || clip.bottom - clip.top < GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// A background process may not take the foreground. Windows lets it right after an Alt tap
// (the window that had the focus gets the tap, and loses the focus at once).
void bringToFront(HWND h, DWORD pid) {
    if (ours(pid)) return;
    SetForegroundWindow(h);
    if (ours(pid)) return;
    INPUT alt[2]{};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(h);
}

std::vector<int> list(const char* s) {
    std::vector<int> out;
    for (const char* p = s; *p;) { out.push_back(std::atoi(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
    return out;
}
}

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: KbmSweep <pid> <out.csv> [--wait ms] [--ms ms] [--period ms] [--speeds a,b] [--rates a,b]"); return 2; }
    const DWORD pid = DWORD(std::strtoul(argv[1], nullptr, 10));
    int wait = 1000, length = 200, period = 1000;
    std::vector<int> speeds{250, 500, 1000, 2000, 4000}, rates{1000, 125};
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--wait")) wait = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--ms")) length = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--period")) period = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--speeds")) speeds = list(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--rates")) rates = list(argv[i + 1]);
    }
    Find f{pid, nullptr, 0};
    for (int i = 0; i < 240 && !f.best; ++i) { EnumWindows(pick, LPARAM(&f)); if (!f.best) Sleep(250); }
    if (!f.best) { std::puts("no window for that process"); return 3; }
    bringToFront(f.best, pid);
    std::printf("window in front=%d\n", int(ours(pid)));
    if (wait < 0) return ours(pid) ? 0 : 1;   // --wait -1: only bring the window to front
    // The app captures once its PS5 picture shows (--ps5-kbm); keep it in front until then.
    for (int i = 0; i < 360 && !(ours(pid) && captured()); ++i) { if (!ours(pid)) bringToFront(f.best, pid); Sleep(250); }
    std::printf("capturing=%d\n", int(ours(pid) && captured()));
    timeBeginPeriod(1);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto sleepUntil = [&](long long t) {
        for (long long left = t - now100ns(); left > 0; left = t - now100ns()) {
            LARGE_INTEGER due{};
            due.QuadPart = -left;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 2000);
        }
    };
    sleepUntil(now100ns() + wait * 10000LL);

    struct Row { long long t; int sweep, speed, rate, dx; };
    std::vector<Row> rows;
    int sweep = 0, aborted = 0;
    long long start = now100ns();
    for (const int rate : rates) {
        for (const int speed : speeds) {
            for (int direction : {1, -1}) {
                if (!ours(pid) || !captured()) { ++aborted; std::puts("window not in front or not capturing: sweep skipped"); }
                else {
                    const int step = 1000 / rate;   // ms between reports
                    double owed = 0;
                    for (int ms = 0; ms < length && ours(pid) && captured(); ms += step) {
                        sleepUntil(start + ms * 10000LL);
                        owed += double(speed) * step / 1000.0;
                        const int dx = int(owed);
                        owed -= dx;
                        if (!dx) continue;
                        INPUT in{};
                        in.type = INPUT_MOUSE;
                        in.mi.dx = dx * direction;
                        in.mi.dwFlags = MOUSEEVENTF_MOVE;
                        const long long t = now100ns();
                        SendInput(1, &in, sizeof(in));
                        rows.push_back({t, sweep, speed, rate, dx * direction});
                    }
                }
                ++sweep;
                start += period * 10000LL;
                sleepUntil(start);
            }
        }
    }
    timeEndPeriod(1);
    FILE* out = std::fopen(argv[2], "w");
    if (!out) return 4;
    std::fprintf(out, "t100ns,sweep,speed,rate,dx\n");
    for (const auto& r : rows) std::fprintf(out, "%lld,%d,%d,%d,%d\n", r.t, r.sweep, r.speed, r.rate, r.dx);
    std::fclose(out);
    std::printf("sweeps=%d aborted=%d moves=%zu\n", sweep, aborted, rows.size());
    return aborted ? 1 : 0;
}
