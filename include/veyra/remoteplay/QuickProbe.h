#pragma once
// Fork: ask one known PS5 for its power state with a unicast discovery packet. The broadcast
// sweep (discoverLocalPs5) takes about 6 s per call; a console that is on or in rest mode
// answers this in well under a second.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

namespace veyra::remoteplay {

enum class ProbeState { NoAnswer, RestMode, Ready };

inline ProbeState probePs5(const std::string& host, int timeoutMs = 700) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return ProbeState::NoAnswer;
    ProbeState result = ProbeState::NoAnswer;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        DWORD timeout = DWORD(timeoutMs);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9302);   // PS5 discovery port
        if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) == 1) {
            static const char request[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
            sendto(s, request, int(sizeof(request) - 1), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            char reply[1024];
            const int n = recv(s, reply, sizeof(reply) - 1, 0);
            if (n > 0) {
                reply[n] = 0;
                const std::string text(reply, size_t(n));
                if (text.rfind("HTTP/1.1 200", 0) == 0) result = ProbeState::Ready;
                else if (text.rfind("HTTP/1.1 620", 0) == 0) result = ProbeState::RestMode;
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return result;
}

}
