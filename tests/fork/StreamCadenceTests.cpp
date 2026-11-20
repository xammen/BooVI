#include "veyra/engine/StreamCadence.h"
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using veyra::engine::StreamCadence;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// One 1080p luma plane: the floor for a new picture is 16 components.
constexpr uint64_t kTotal = 1920 * 1080;
constexpr uint64_t kMove = 400000, kCopy = 3;

// Feeds a pattern of N (new) and R (repeat) and returns K (kept), D (dropped) and H (held) per picture.
static std::string run(StreamCadence& cadence, const std::string& pattern, uint64_t move = kMove, uint64_t copy = kCopy) {
    std::string out;
    for (const char c : pattern) {
        const auto d = cadence.observe(c == 'N' ? move : copy, kTotal);
        out += !d.keep ? 'D' : d.hold ? 'H' : 'K';
    }
    return out;
}
static std::string repeat(const std::string& unit, unsigned times) {
    std::string out;
    for (unsigned i = 0; i < times; ++i) out += unit;
    return out;
}
static unsigned countOf(const std::string& text, char c) {
    unsigned n = 0;
    for (const char x : text) n += x == c;
    return n;
}

int main() {
    {
        StreamCadence c;
        const auto out = run(c, repeat("N", 240));
        expect(countOf(out, 'K') == 240 && c.mode() == StreamCadence::Mode::Full, "60 fps content: every picture kept, mode stays full");
    }
    {
        StreamCadence c;
        const auto out = run(c, repeat("R", 240));
        expect(countOf(out, 'K') == 240 && c.mode() == StreamCadence::Mode::Full, "a still screen: every picture kept, mode stays full");
    }
    {
        StreamCadence c;
        const auto lead = run(c, repeat("NR", 8));
        expect(countOf(lead, 'D') == 0, "30 fps game: nothing dropped during the first 16 pictures");
        const auto out = run(c, repeat("NR", 60));
        expect(c.mode() == StreamCadence::Mode::Half, "30 fps game: half mode after the alternation held");
        expect(out.substr(out.size() - 60) == repeat("KD", 30), "30 fps game: first copies kept, second copies dropped");
        expect(c.stats().entered == 1 && c.stats().left == 0, "30 fps game: entered half once and stayed");
    }
    {
        StreamCadence c;
        run(c, repeat("RN", 40));
        expect(c.mode() == StreamCadence::Mode::Half, "30 fps game on the other phase: half mode too");
    }
    {
        // The pause menu measured on the console: a new picture every 4-6 pictures.
        StreamCadence c;
        const auto out = run(c, repeat("NRRRNRRRRRNRRRRN", 30), 128, 1);
        expect(countOf(out, 'D') == 0 && c.mode() == StreamCadence::Mode::Full, "slow drifting menu: never half, nothing dropped");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 40));
        const auto out = run(c, "NRRNRNR");
        expect(out == "KDDKDKD" && c.mode() == StreamCadence::Mode::Half, "a 50 ms game frame: both copies of the late picture dropped, half kept");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 40));
        const auto out = run(c, "NNRNR" + repeat("NR", 20));
        expect(c.mode() == StreamCadence::Mode::Half && countOf(out, 'K') == 23, "one catch-up pair of new pictures: both kept, half kept");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 40));
        const auto out = run(c, repeat("N", 12));
        expect(c.mode() == StreamCadence::Mode::Full && countOf(out, 'D') == 0, "game goes to 60 fps: full again after four new pictures in a row, no picture lost");
        const auto soon = run(c, repeat("NR", 12));
        expect(c.mode() == StreamCadence::Mode::Full && countOf(soon, 'D') == 0, "after leaving half: 24 alternating pictures are not enough to return");
        run(c, repeat("NR", 10));
        expect(c.mode() == StreamCadence::Mode::Half, "after leaving half: it returns once the alternation lasted twice as long");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 40));
        const auto out = run(c, repeat("R", 12));
        expect(out == "DDHDHDHDHDHD", "paused game: the still picture is refreshed 4 pictures after the last new one, then every second picture");
        const auto resumed = run(c, repeat("NR", 6));
        expect(resumed == repeat("KD", 6) && c.mode() == StreamCadence::Mode::Half, "game resumes: half continues without a new search");
    }
    {
        // An encoder that still refines the second copy: 3000 components differ in a repeat.
        StreamCadence c;
        const auto out = run(c, repeat("NR", 60), kMove, 3000);
        expect(c.mode() == StreamCadence::Mode::Half && out.substr(out.size() - 20) == repeat("KD", 10), "noisy second copies: judged against the new picture before them, still dropped");
        run(c, repeat("R", 40), kMove, 0);
        const auto after = run(c, repeat("NR", 10), kMove, 3000);
        expect(after == repeat("KD", 10), "noisy second copies after a still screen: the still taught no noise level");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 40));
        const auto d = c.blind();
        expect(d.keep && c.mode() == StreamCadence::Mode::Full, "a picture without a measurement: kept, back to full");
    }
    {
        // A 60 fps picture whose changes alternate large and small by less than the ratio.
        StreamCadence c;
        std::string out;
        for (unsigned i = 0; i < 240; ++i) out += c.observe(i % 2 ? 160000 : 400000, kTotal).keep ? 'K' : 'D';
        expect(countOf(out, 'D') == 0 && c.mode() == StreamCadence::Mode::Full, "uneven 60 fps motion: not mistaken for copies");
    }
    {
        // The reader misses second copies now and then: each compare after a miss spans two pictures.
        StreamCadence c;
        run(c, repeat("NR", 40));
        std::string out;
        for (unsigned i = 0; i < 20; ++i) { out += run(c, "NR"); c.gap(); out += run(c, "N"); out += run(c, "R"); }
        expect(c.mode() == StreamCadence::Mode::Half && countOf(out, 'D') == 40, "missed pictures: a compare across a gap does not end half mode");
    }
    {
        // In full mode the count arrives after the picture was shown: the copy that completes the
        // alternation cannot be dropped any more, the ones after it are.
        StreamCadence c;
        std::string out;
        for (unsigned i = 0; i < 18; ++i) out += c.observe(i % 2 ? kCopy : kMove, kTotal, true).keep ? 'K' : 'D';
        expect(countOf(out, 'D') == 0 && c.mode() == StreamCadence::Mode::Half, "counts read late: half is entered without dropping the shown picture");
        expect(run(c, repeat("NR", 6)) == repeat("KD", 6), "counts read late: the following second copies are dropped");
    }
    {
        // Change counts of a 30 fps PS5 game, 1080p HEVC, read off the console on 2026-11-20: the
        // copies are not equal to their first pictures, and now and then one changes 81 or 171 samples.
        const uint64_t measured[] = {12982, 0, 12333, 0, 6248, 2, 29321, 1, 36962, 6, 70996, 14, 93408, 12, 55835, 3,
            22007, 19, 14337, 4, 37670, 20, 51068, 81, 76766, 27, 11814, 2, 12284, 1, 15496, 1, 15178, 20, 18267, 8,
            26684, 2, 60519, 171, 121929, 49, 17165, 26, 4462, 42, 11664, 36, 5967, 16, 3832, 63, 52730, 48, 52819, 71};
        StreamCadence c;
        run(c, repeat("NR", 20));
        std::string out;
        for (unsigned pass = 0; pass < 6; ++pass)
            for (const auto changed : measured) out += c.observe(changed, kTotal).keep ? 'K' : 'D';
        expect(c.mode() == StreamCadence::Mode::Half && c.stats().left == 0 && out == repeat("KD", unsigned(out.size() / 2)),
               "measured PS5 game: every refined copy dropped, half never left");
    }
    {
        // After a still screen the game moves slowly: the copy before a new picture is the still
        // itself, so the small real changes are kept.
        StreamCadence c;
        run(c, repeat("NR", 40));
        run(c, repeat("R", 30), kMove, 0);
        const auto out = run(c, repeat("NR", 8), 300, 2);
        expect(out == repeat("KD", 8), "slow motion after fast motion and a still: the small new pictures are kept");
    }
    {
        // A busier scene of the same game: the encoder refines each copy by 800 to 2 300 samples,
        // once by 5 638, a quarter of the new picture before it.
        const uint64_t measured[] = {22315, 1038, 19071, 809, 18850, 758, 19817, 780, 17638, 834, 17912, 1098, 18852, 1555,
            20440, 1467, 66642, 1382, 23873, 5638, 20789, 637, 21904, 1149, 21232, 1638, 20931, 711, 24416, 615,
            17409, 495, 16523, 101, 19701, 495, 18464, 402, 17462, 1991, 18653, 688, 21903, 305, 21685, 2306,
            1011166, 3976, 1060279, 2665, 1051394, 639, 1006177, 25890};
        StreamCadence c;
        std::string out;
        for (unsigned pass = 0; pass < 6; ++pass)
            for (const auto changed : measured) out += c.observe(changed, kTotal).keep ? 'K' : 'D';
        const auto steady = out.substr(sizeof measured / sizeof *measured);
        expect(c.mode() == StreamCadence::Mode::Half && c.stats().left == 0 && steady == repeat("KD", unsigned(steady.size() / 2)),
               "measured busy scene: copies refined by thousands of samples are dropped, half never left");
    }
    {
        // The game misses its frame time: a torn picture, the rest of it, then the next one, three
        // new pictures in a row (measured: 25815, 23893, 38068). The alternation goes on one slot later.
        StreamCadence c;
        run(c, repeat("NR", 40));
        std::string out;
        for (unsigned i = 0; i < 12; ++i) {
            for (const uint64_t changed : {25815, 23893, 38068}) out += c.observe(changed, kTotal).keep ? 'K' : 'D';
            out += run(c, "R" + repeat("NR", 9), 22000, 900);
        }
        expect(c.mode() == StreamCadence::Mode::Half && c.stats().left == 0 && countOf(out, 'D') == 12 * 10,
               "torn frames every 22 pictures: all three pictures kept, half never left");
    }
    {
        StreamCadence c;
        run(c, repeat("NR", 20));
        uint64_t news = 0;
        for (unsigned i = 0; i < 60; ++i) news += c.observe(i % 2 ? 1500 : 20000, kTotal).newPicture;
        StreamCadence full;
        uint64_t before = 0;
        for (unsigned i = 0; i < 14; ++i) before += full.observe(i % 2 ? 1500 : 20000, kTotal, true).newPicture;
        expect(news == 30 && before == 7 && full.mode() == StreamCadence::Mode::Full,
               "content rate: a refined copy is not counted as a new picture, before half mode either");
    }
    std::printf(failures ? "FAILED %d\n" : "all stream cadence tests passed\n", failures);
    return failures ? 1 : 0;
}
