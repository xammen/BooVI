#include "veyra/guest/GuestNotify.h"
#include <cstdio>

using namespace veyra::guest;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    {
        RoomEvent taken{RoomEvent::Kind::PadTaken, "lucas", {}, Handoff::Dice};
        expect(noticeFor(taken)->text == "🎮 lucas a la manette · gagné aux dés", "the controller, won at the dice");
        expect(noticeFor({RoomEvent::Kind::Joined, "zoé"})->text == "zoé est connecté(e)", "connected");
        expect(noticeFor({RoomEvent::Kind::PadFree, {}})->kind == Notice::PadFree, "ps5 libre");
        expect(noticeFor({RoomEvent::Kind::DiceRolled, "sam", "lucas"})->text == "🎲 sam gagne aux dés (lancés par lucas)", "dice result");
        expect(!noticeFor({RoomEvent::Kind::SeatChanged, "sam"}), "seat changes stay in the app");
        expect(discordName("@everyone") == "everyone", "no mention through a name");
        expect(discordName("**x**_y") == R"(\*\*x\*\*\_y)", "no markdown through a name");
        expect(webhookBody("a \"b\"\n") == R"({"username":"BooVI","content":"a \"b\"\n","allowed_mentions":{"parse":[]}})", "the body escapes and pings nobody");
    }
    {
        const char* good = "https://discord.com/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz-_ABCDEFGHIJ";
        expect(webhookUrlOk(good), "a discord webhook");
        expect(!webhookUrlOk("https://discord.com.evil.example/api/webhooks/1/abcdefghijklmnopqrstuvwxyz"), "another host");
        expect(!webhookUrlOk("http://discord.com/api/webhooks/123/abcdefghijklmnopqrstuvwxyz"), "not https");
        expect(!webhookUrlOk("https://discord.com/api/webhooks/12a/abcdefghijklmnopqrstuvwxyz"), "the id is digits");
        expect(!webhookUrlOk("https://discord.com/api/webhooks/123/abc?x=1&wait=true#frag"), "nothing after the token");
        expect(!webhookUrlOk("http://127.0.0.1:9/hook") && webhookUrlOk("http://127.0.0.1:9/hook", true), "loopback only for tests");
    }
    {
        NotifyGate g;
        g.push("a", 0);
        expect(!g.take(1000), "a line waits for its burst");
        g.push("b", 1200);
        g.push("b", 1300);
        const auto first = g.take(1500);
        expect(first && *first == "a\nb", "a burst goes out as one message, a repeated line once");
        g.push("c", 1600);
        expect(!g.take(3200), "3 s between two messages");
        expect(g.take(4500) == std::optional<std::string>("c"), "then the next one");
        int sent = 0;
        for (int64_t t = 10000; t < 70000; t += 250) { g.push("x" + std::to_string(t), t); if (g.take(t)) ++sent; }
        expect(sent >= 18 && sent <= 20, "a steady stream: at most 20 messages a minute");
    }
    {
        NotifyGate g;
        for (int i = 0; i < 30; ++i) g.push("line " + std::to_string(i), 0);
        const auto m = g.take(2000);
        expect(m && m->find("line 11") != std::string::npos && m->find("line 12") == std::string::npos && m->ends_with("… et 18 de plus"), "a long burst is cut and counted");
        expect(g.pending() == 0, "nothing left behind");
    }
    {
        NotifyGate g;
        g.push("a", 0);
        const auto m = g.take(2000);
        g.rateLimited(*m, 5000, 2100);
        expect(!g.take(6000), "retry_after is honoured");
        expect(g.take(7200) == std::optional<std::string>("a"), "and the message goes again");
    }
    expect((kDefaultNotices & noticeBit(Notice::ShareStarted)) == 0 && (kDefaultNotices & noticeBit(Notice::PadFree)), "defaults: ps5 libre on, share started off");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
