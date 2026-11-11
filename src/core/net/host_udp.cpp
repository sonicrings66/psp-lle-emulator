#include "host_udp.h"

#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kBadSocket = INVALID_SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBadSocket = -1;
static void closeSocket(socket_t s) { ::close(s); }
#endif

namespace beta::net {
namespace {

struct HostUdp : UdpProxy::HostSock {
    std::mutex m;
    socket_t s = kBadSocket;
    bool closed = false;

    void send(const uint8_t* d, std::size_t n) override {
        std::lock_guard<std::mutex> lock(m);
        if (!closed && s != kBadSocket) ::send(s, reinterpret_cast<const char*>(d), static_cast<int>(n), 0);
    }
    void close() override {
        std::lock_guard<std::mutex> lock(m);
        if (closed) return;
        closed = true;
        if (s != kBadSocket) closeSocket(s);
    }
    bool isClosed() { std::lock_guard<std::mutex> lock(m); return closed; }
};

void startSockets() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); });
#endif
}

} // namespace

std::shared_ptr<UdpProxy::HostSock> hostUdpOpen(uint32_t ip, uint16_t port, UdpProxy::OnData onData) {
    startSockets();
    const socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kBadSocket) return nullptr;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closeSocket(sock); return nullptr; }
    auto hs = std::make_shared<HostUdp>();
    hs->s = sock;
    std::thread([hs, sock, onData] {
        uint8_t buf[2048];
        while (!hs->isClosed()) {
            fd_set r; FD_ZERO(&r); FD_SET(sock, &r);
            timeval tv{0, 200000};
            const int ready = ::select(static_cast<int>(sock) + 1, &r, nullptr, nullptr, &tv);
            if (ready < 0) break;
            if (ready == 0) continue;
            const int n = ::recv(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n < 0) { if (hs->isClosed()) break; continue; }
            if (onData) onData(buf, static_cast<std::size_t>(n));
        }
    }).detach();
    return hs;
}

} // namespace beta::net
