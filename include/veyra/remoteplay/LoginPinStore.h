#pragma once
// Fork: the PS5 account's login PIN, remembered only when the user turns it on. Encrypted with
// DPAPI for the current Windows user (as the pairing profile is) in its own file next to the
// profile, so the shared profile format stays readable by upstream Veyra.
#include <windows.h>
#include <wincrypt.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace veyra::remoteplay {

inline std::filesystem::path loginPinPath(const std::filesystem::path& profile) {
    auto p = profile;
    p += L".loginpin";
    return p;
}

inline bool saveLoginPin(const std::filesystem::path& profile, const std::string& pin) {
    if (pin.empty() || pin.size() > 16) return false;
    for (char c : pin) if (c < '0' || c > '9') return false;
    DATA_BLOB in{DWORD(pin.size()), reinterpret_cast<BYTE*>(const_cast<char*>(pin.data()))}, out{};
    if (!CryptProtectData(&in, L"Veyra PS5 login PIN", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::ofstream file(loginPinPath(profile), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return bool(file);
}

inline std::optional<std::string> loadLoginPin(const std::filesystem::path& profile) {
    std::ifstream file(loginPinPath(profile), std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) return std::nullopt;
    DATA_BLOB in{DWORD(data.size()), reinterpret_cast<BYTE*>(data.data())}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::string pin(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    for (char c : pin) if (c < '0' || c > '9') return std::nullopt;
    return pin.empty() ? std::nullopt : std::optional<std::string>(std::move(pin));
}

inline void forgetLoginPin(const std::filesystem::path& profile) {
    std::error_code ec;
    std::filesystem::remove(loginPinPath(profile), ec);
}

}
