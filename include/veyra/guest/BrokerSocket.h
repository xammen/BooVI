#pragma once
// Fork, partage: one WebSocket to BooVI's broker (GuestRoom.h), over WinHTTP so the system's
// certificate roots and proxy apply. The host's room (src/guest/GuestBroker.cpp) and the
// native client's join (src/source/GuestClientSource.cpp) both use it. Text messages only.
// send() and close() may be called from any thread while another one waits in receive().
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <format>
#include <mutex>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace veyra::guest {

class BrokerSocket {
public:
    BrokerSocket() = default;
    BrokerSocket(const BrokerSocket&) = delete;
    BrokerSocket& operator=(const BrokerSocket&) = delete;
    ~BrokerSocket() { close(); }

    // `headers`: extra request headers, each ending in \r\n. The handshake's HTTP status is in
    // `status` (101 on success).
    bool open(const std::wstring& host, uint16_t port, bool secure, const std::wstring& path, const std::wstring& headers,
              int& status, std::string& error) {
        close();
        HINTERNET session = WinHttpOpen(L"BooVI", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        // Receive waits forever: a room is quiet between guests. Callers bound it with close().
        if (session) WinHttpSetTimeouts(session, 5000, 5000, 5000, 0);
        HINTERNET connection = session ? WinHttpConnect(session, host.c_str(), port, 0) : nullptr;
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        {
            std::lock_guard lock(m_);
            session_ = session; connection_ = connection; request_ = request;
        }
        status = 0;
        const bool sent = request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
                       && WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1),
                                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                       && WinHttpReceiveResponse(request, nullptr);
        if (!sent) { error = std::format("winhttp {}", GetLastError()); close(); return false; }
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = int(code);
        if (code != 101) { error = std::format("http {}", code); close(); return false; }
        HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (!socket) { error = std::format("upgrade {}", GetLastError()); close(); return false; }
        std::lock_guard lock(m_);
        WinHttpCloseHandle(request_);
        request_ = nullptr;
        socket_ = socket;
        return true;
    }

    bool send(const std::string& text) {
        std::lock_guard lock(sendMutex_);
        HINTERNET socket = handle();
        return socket && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()), DWORD(text.size())) == NO_ERROR;
    }

    // One whole message. False once the socket is closed, by either side; `closeCode` is the
    // broker's close code when it sent one (4404: the host left, 4408: the room expired…).
    bool receive(std::string& text, uint16_t& closeCode) {
        text.clear();
        closeCode = 0;
        char buffer[16 * 1024];
        for (;;) {
            HINTERNET socket = handle();
            if (!socket) return false;
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            if (WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &read, &type) != NO_ERROR) return false;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status = 0;
                char reason[128];
                DWORD length = 0;
                if (WinHttpWebSocketQueryCloseStatus(socket, &status, reason, sizeof(reason), &length) == NO_ERROR) closeCode = status;
                return false;
            }
            text.append(buffer, read);
            if (text.size() > (1u << 20)) return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return true;
        }
    }

    // Any thread: a pending receive() returns false.
    void close() {
        std::lock_guard lock(m_);
        for (HINTERNET* h : {&socket_, &request_, &connection_, &session_}) {
            if (*h) WinHttpCloseHandle(*h);
            *h = nullptr;
        }
    }

private:
    HINTERNET handle() { std::lock_guard lock(m_); return socket_; }

    std::mutex m_, sendMutex_;
    HINTERNET session_ = nullptr, connection_ = nullptr, request_ = nullptr, socket_ = nullptr;
};

} // namespace veyra::guest
