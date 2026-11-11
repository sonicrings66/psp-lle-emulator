#include "host_tcp.h"

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
static const int kShutWrite = SD_SEND, kShutBoth = SD_BOTH;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBadSocket = -1;
static void closeSocket(socket_t s) { ::close(s); }
static const int kShutWrite = SHUT_WR, kShutBoth = SHUT_RDWR;
#endif

namespace beta::net {
namespace {

struct HostSocket : TcpProxy::HostConn {
    std::mutex m;
    socket_t s = kBadSocket;
    bool closed = false;

    void send(const uint8_t* d, std::size_t n) override {
        std::lock_guard<std::mutex> lock(m);
        if (closed || s == kBadSocket) return;
        std::size_t off = 0;
        while (off < n) {
            const int r = ::send(s, reinterpret_cast<const char*>(d + off), static_cast<int>(n - off), 0);
            if (r <= 0) break;
            off += static_cast<std::size_t>(r);
        }
    }
    void shutdownWrite() override { std::lock_guard<std::mutex> lock(m); if (!closed && s != kBadSocket) ::shutdown(s, kShutWrite); }
    void close() override {
        std::lock_guard<std::mutex> lock(m);
        if (closed) return;
        closed = true;
        if (s != kBadSocket) { ::shutdown(s, kShutBoth); closeSocket(s); }
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

std::shared_ptr<TcpProxy::HostConn> hostTcpConnect(uint32_t ip, uint16_t port, TcpProxy::HostEvents events) {
    startSockets();
    auto hs = std::make_shared<HostSocket>();
    std::thread([hs, ip, port, events] {
        const socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kBadSocket) { if (events.connected) events.connected(false); return; }
        {
            std::lock_guard<std::mutex> lock(hs->m);
            if (hs->closed) { closeSocket(sock); return; }
            hs->s = sock;
        }
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
        // connect without blocking forever: non-blocking connect, then wait up to 8 seconds
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
        bool ok = false;
        const int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (rc == 0) ok = true;
        else {
            fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(sock, &w); FD_SET(sock, &e);
            timeval tv{8, 0};
            if (::select(static_cast<int>(sock) + 1, nullptr, &w, &e, &tv) > 0 && FD_ISSET(sock, &w)) {
                int err = 0; socklen_t len = sizeof(err);
                ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                ok = err == 0;
            }
        }
#ifdef _WIN32
        nb = 0; ioctlsocket(sock, FIONBIO, &nb);
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK);
#endif
        if (events.connected) events.connected(ok);
        if (!ok) { hs->close(); return; }
        uint8_t buf[8192];
        while (!hs->isClosed()) {
            fd_set r; FD_ZERO(&r); FD_SET(sock, &r);
            timeval tv{0, 200000};
            const int ready = ::select(static_cast<int>(sock) + 1, &r, nullptr, nullptr, &tv);
            if (ready < 0) break;
            if (ready == 0) continue;
            const int n = ::recv(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            if (events.data) events.data(buf, static_cast<std::size_t>(n));
        }
        if (!hs->isClosed() && events.closed) events.closed();
    }).detach();
    return hs;
}

} // namespace beta::net
