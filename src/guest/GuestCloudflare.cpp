#include "GuestCloudflare.h"
#include "veyra/guest/GuestStreamer.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include "veyra/UserDataRoot.h"
#include "veyra/guest/GuestRoom.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace veyra::guest {
namespace {

std::filesystem::path guestFile(const wchar_t* name) {
    try { return veyra::userdata::root() / L"guest" / name; } catch (const std::exception&) { return {}; }
}

// The file's plain text, empty when it is missing or not sealed for this user.
std::string unseal(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<char> sealed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (sealed.empty()) return {};
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(sealed.data())}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string text(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return text;
}

bool seal(const std::filesystem::path& path, const std::string& text) {
    DATA_BLOB in{DWORD(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, out{};
    if (path.empty() || !CryptProtectData(&in, L"BooVI", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += L".tmp";
    bool ok = false;
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        ok = file && file.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    }
    LocalFree(out.pbData);
    if (ok) std::filesystem::rename(temp, path, ec);
    return ok && !ec;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
    return out;
}

bool plainId(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return false;
    return true;
}

struct Internet {
    HINTERNET h = nullptr;
    ~Internet() { if (h) WinHttpCloseHandle(h); }
};

} // namespace

std::optional<CloudflareConfig> loadCloudflareConfig() {
    std::string text = unseal(guestFile(L"cloudflare.dpapi"));
    const auto json = nlohmann::json::parse(text, nullptr, false);
    SecureZeroMemory(text.data(), text.size());
    auto str = [&](const char* key) { return json.is_object() && json.contains(key) && json[key].is_string() ? json[key].get<std::string>() : std::string(); };
    CloudflareConfig config{str("ownerKey"), str("turnKeyId"), str("turnApiToken")};
    if (config.ownerKey.empty() && (config.turnKeyId.empty() || config.turnApiToken.empty())) return std::nullopt;
    return config;
}

std::string loadOrCreateRoomKey() {
    const auto path = guestFile(L"room.dpapi");
    if (std::string key = unseal(path); isRoomKey(key)) return key;
    std::string key = newRoomKey();
    if (!seal(path, key)) return {};
    return key;
}

std::string loadDiscordWebhook() { return unseal(guestFile(L"discord.dpapi")); }

bool saveDiscordWebhook(const std::string& url) { return seal(guestFile(L"discord.dpapi"), url); }

void clearDiscordWebhook() {
    std::error_code ec;
    if (const auto path = guestFile(L"discord.dpapi"); !path.empty()) std::filesystem::remove(path, ec);
}

nlohmann::json fetchTurnIceServers(const CloudflareConfig& config, int ttlSeconds, std::string& error) {
    auto empty = nlohmann::json::array();
    if (!plainId(config.turnKeyId) || config.turnApiToken.empty()) { error = "pas de clé turn"; return empty; }
    Internet session{WinHttpOpen(L"BooVI", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) { error = "winhttp"; return empty; }
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 5000);
    Internet connection{WinHttpConnect(session.h, L"rtc.live.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    const std::wstring path = L"/v1/turn/keys/" + widen(config.turnKeyId) + L"/credentials/generate-ice-servers";
    Internet request{connection.h ? WinHttpOpenRequest(connection.h, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
    if (!request.h) { error = "winhttp"; return empty; }
    std::wstring headers = L"Authorization: Bearer " + widen(config.turnApiToken) + L"\r\nContent-Type: application/json\r\n";
    std::string body = std::format(R"({{"ttl":{}}})", ttlSeconds);
    const BOOL sent = WinHttpSendRequest(request.h, headers.c_str(), DWORD(-1), body.data(), DWORD(body.size()), DWORD(body.size()), 0)
                   && WinHttpReceiveResponse(request.h, nullptr);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent) { error = std::format("turn injoignable ({})", GetLastError()); return empty; }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string reply;
    for (DWORD available = 0; WinHttpQueryDataAvailable(request.h, &available) && available && reply.size() < 64 * 1024;) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read) || !read) break;
        reply.append(chunk.data(), read);
    }
    if (status < 200 || status >= 300) { error = std::format("turn a répondu {}", status); return empty; }
    auto parsed = nlohmann::json::parse(reply, nullptr, false);
    SecureZeroMemory(reply.data(), reply.size());
    if (!parsed.is_object() || !parsed.contains("iceServers") || !parsed["iceServers"].is_array()) { error = "réponse turn illisible"; return empty; }
    return parsed["iceServers"];
}

} // namespace veyra::guest
