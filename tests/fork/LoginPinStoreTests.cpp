#include "veyra/remoteplay/LoginPinStore.h"
#include <cstdio>

using namespace veyra::remoteplay;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const auto profile = std::filesystem::temp_directory_path() / L"veyra-fork-pin-test.dat";
    forgetLoginPin(profile);
    expect(!loadLoginPin(profile), "nothing stored: no PIN");
    expect(saveLoginPin(profile, "4821"), "a 4-digit PIN is stored");
    expect(loadLoginPin(profile) == std::optional<std::string>("4821"), "the stored PIN reads back");
    {
        std::ifstream f(loginPinPath(profile), std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        expect(raw.find("4821") == std::string::npos, "the file does not hold the PIN in clear");
    }
    {
        std::fstream f(loginPinPath(profile), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(40); f.put('\x5a');
    }
    expect(!loadLoginPin(profile), "a tampered file is rejected");
    expect(!saveLoginPin(profile, "12a4") && !saveLoginPin(profile, ""), "only digits are accepted");
    forgetLoginPin(profile);
    expect(!std::filesystem::exists(loginPinPath(profile)), "forgetting removes the file");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
