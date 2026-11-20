#pragma once
// Where BooVI keeps what belongs to the Windows user rather than to one copy of the app:
// PS5 pairings, Moonlight and Xbox identities, the Cloudflare sharing secrets, the QML cache.
// Every store resolves its folder here: %LOCALAPPDATA%\BooVI, or BOOVI_USER_DATA when set
// (tests and side-by-side test copies, so they never share the user's pairings).
//
// Settings and presets stay next to the exe (runtime_local, RuntimePaths.h): the portable folder.
//
// Before BooVI the app was Veyra and used %LOCALAPPDATA%\Veyra. migrateLegacyUserData copies
// the stores from there once, the first time BooVI runs, and never moves or deletes anything.

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace veyra::userdata {

// The stores the Veyra-era app kept under %LOCALAPPDATA%\Veyra. Its Qt cache (Veyra\Veyra) is
// left behind: it is rebuilt on demand.
inline constexpr std::array<std::wstring_view, 4> kLegacyStores{L"remoteplay", L"moonlight", L"xbox", L"guest"};

inline std::filesystem::path localAppData() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw std::runtime_error("Cannot resolve the user data directory");
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

inline bool overridden() {
    return GetEnvironmentVariableW(L"BOOVI_USER_DATA", nullptr, 0) > 0;
}

inline std::filesystem::path root() {
    wchar_t forced[MAX_PATH]{};
    if (const DWORD n = GetEnvironmentVariableW(L"BOOVI_USER_DATA", forced, MAX_PATH); n > 0 && n < MAX_PATH)
        return std::filesystem::path(forced);
    return localAppData() / L"BooVI";
}

inline std::filesystem::path legacyRoot() { return localAppData() / L"Veyra"; }

enum class Migration { NothingToCopy, AlreadyDone, Copied, Failed };

// One-time copy of the Veyra-era stores into a BooVI folder that does not exist yet. The copy
// is assembled beside the target and renamed into place, so a crash leaves either no BooVI
// folder (the next start tries again) or a complete one. The legacy folder is only read.
inline Migration migrateLegacyUserData(const std::filesystem::path& legacy, const std::filesystem::path& current,
                                       std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(current, ec)) return Migration::AlreadyDone;
    if (ec) { error = "cannot inspect " + current.string() + ": " + ec.message(); return Migration::Failed; }
    bool any = false;
    for (const auto store : kLegacyStores) any = any || fs::is_directory(legacy / store, ec);
    if (!any) return Migration::NothingToCopy;

    auto staging = current;
    staging += L".migrating";
    fs::remove_all(staging, ec);   // a copy an earlier start did not finish
    ec.clear();
    fs::create_directories(staging, ec);
    if (ec) { error = "cannot create " + staging.string() + ": " + ec.message(); return Migration::Failed; }
    for (const auto store : kLegacyStores) {
        const auto from = legacy / store;
        if (!fs::is_directory(from, ec)) continue;
        fs::copy(from, staging / store, fs::copy_options::recursive, ec);
        if (ec) { error = "cannot copy " + from.string() + ": " + ec.message(); return Migration::Failed; }
    }
    fs::rename(staging, current, ec);
    if (ec) { error = "cannot install " + current.string() + ": " + ec.message(); return Migration::Failed; }
    return Migration::Copied;
}

} // namespace veyra::userdata
