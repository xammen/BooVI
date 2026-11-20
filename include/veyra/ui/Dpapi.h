#pragma once
// Fork: small secrets kept on disk for this Windows user only (DPAPI): a host's friends'
// accesses (GuestAccessModel.h), a friend's saved access link (GuestJoinModel.h).
#include <windows.h>
#include <wincrypt.h>

#include <QByteArray>

#pragma comment(lib, "crypt32.lib")

namespace veyra::ui {

inline QByteArray dpapiSeal(const QByteArray& plain) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, out{};
    if (!CryptProtectData(&in, L"BooVI", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray sealed(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return sealed;
}

inline QByteArray dpapiUnseal(const QByteArray& sealed) {
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))}, out{};
    if (sealed.isEmpty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray plain(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace veyra::ui
