// The one-time copy of the Veyra-era user data into BooVI's folder (UserDataRoot.h), on fake
// folders under %TEMP%: never the user's real %LOCALAPPDATA%.
#include "veyra/UserDataRoot.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using veyra::userdata::Migration;
using veyra::userdata::migrateLegacyUserData;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void write(const fs::path& file, const char* text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

static std::string read(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main() {
    const fs::path base = fs::temp_directory_path() / L"boovi-userdata-migration-test";
    fs::remove_all(base);
    const fs::path legacy = base / L"Veyra", current = base / L"BooVI";
    std::string error;

    expect(migrateLegacyUserData(legacy, current, error) == Migration::NothingToCopy && !fs::exists(current),
           "no Veyra folder: nothing copied, no BooVI folder made");

    write(legacy / L"remoteplay" / L"profiles.dat", "pairing");
    write(legacy / L"guest" / L"cloudflare.dpapi", "secret");
    write(legacy / L"Veyra" / L"cache" / L"big.qmlc", "cache");
    expect(migrateLegacyUserData(legacy, current, error) == Migration::Copied, "a Veyra folder and no BooVI folder: copied");
    expect(read(current / L"remoteplay" / L"profiles.dat") == "pairing", "the PS5 pairing is in the BooVI folder");
    expect(read(current / L"guest" / L"cloudflare.dpapi") == "secret", "the sharing secrets are in the BooVI folder");
    expect(!fs::exists(current / L"Veyra"), "the old Qt cache is left behind");
    expect(read(legacy / L"remoteplay" / L"profiles.dat") == "pairing" && fs::exists(legacy / L"guest" / L"cloudflare.dpapi"),
           "the Veyra folder is untouched");
    expect(!fs::exists(base / L"BooVI.migrating"), "no staging folder is left");

    write(current / L"remoteplay" / L"profiles.dat", "new pairing");
    write(legacy / L"remoteplay" / L"profiles.dat", "older pairing");
    expect(migrateLegacyUserData(legacy, current, error) == Migration::AlreadyDone, "a BooVI folder exists: nothing copied");
    expect(read(current / L"remoteplay" / L"profiles.dat") == "new pairing", "BooVI's own data is never overwritten");

    fs::remove_all(current);
    write(base / L"BooVI.migrating" / L"remoteplay" / L"half.dat", "partial");
    expect(migrateLegacyUserData(legacy, current, error) == Migration::Copied, "an unfinished earlier copy: copied again");
    expect(!fs::exists(current / L"remoteplay" / L"half.dat") && read(current / L"remoteplay" / L"profiles.dat") == "older pairing",
           "the unfinished copy is replaced, not merged");

    fs::remove_all(base);
    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
