#include "host_resolver.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static void closeSocket(socket_t s) { ::close(s); }
#endif

namespace beta::net {

std::vector<uint8_t> buildDnsQuery(const std::string& name, uint16_t id) {
    std::vector<uint8_t> q(12, 0);
    q[0] = static_cast<uint8_t>(id >> 8); q[1] = static_cast<uint8_t>(id);
    q[2] = 0x01;                      // standard query, recursion desired
    q[5] = 1;                         // one question
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t dot = name.find('.', start), end = dot == std::string::npos ? name.size() : dot;
        if (end > start) { q.push_back(static_cast<uint8_t>(end - start)); for (std::size_t i = start; i < end; ++i) q.push_back(static_cast<uint8_t>(name[i])); }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    q.push_back(0);
    q.push_back(0); q.push_back(1);   // type A
    q.push_back(0); q.push_back(1);   // class IN
    return q;
}

namespace {
// Skips a (possibly compressed) name; returns the offset after it, or 0 on a malformed message.
std::size_t skipName(const uint8_t* m, std::size_t n, std::size_t i) {
    while (i < n) {
        const unsigned l = m[i];
        if (l == 0) return i + 1;
        if ((l & 0xC0) == 0xC0) return i + 2 <= n ? i + 2 : 0;
        i += 1 + l;
    }
    return 0;
}
} // namespace

bool parseFirstARecord(const uint8_t* m, std::size_t n, uint32_t& ip) {
    if (n < 12 || !(m[2] & 0x80) || (m[3] & 0x0F) != 0) return false;   // a response without an error
    const unsigned qd = (static_cast<unsigned>(m[4]) << 8) | m[5], an = (static_cast<unsigned>(m[6]) << 8) | m[7];
    std::size_t i = 12;
    for (unsigned k = 0; k < qd; ++k) { i = skipName(m, n, i); if (!i) return false; i += 4; }
    for (unsigned k = 0; k < an; ++k) {
        i = skipName(m, n, i);
        if (!i || i + 10 > n) return false;
        const unsigned type = (static_cast<unsigned>(m[i]) << 8) | m[i + 1];
        const unsigned rdlen = (static_cast<unsigned>(m[i + 8]) << 8) | m[i + 9];
        i += 10;
        if (i + rdlen > n) return false;
        if (type == 1 && rdlen == 4) { ip = (static_cast<uint32_t>(m[i]) << 24) | (static_cast<uint32_t>(m[i + 1]) << 16) | (static_cast<uint32_t>(m[i + 2]) << 8) | m[i + 3]; return true; }
        i += rdlen;
    }
    return false;
}

namespace {

struct Job { std::string name; std::function<void(bool, uint32_t)> done; uint32_t upstream = 0; };

// Asks `server` directly (UDP port 53): one retry after 2 seconds, then gives up.
bool lookupViaServer(const std::string& name, uint32_t server, uint32_t& ip) {
    const socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == static_cast<socket_t>(-1)) return false;
    static uint16_t nextId = 0x5A00;
    const uint16_t id = nextId++;
    const std::vector<uint8_t> q = buildDnsQuery(name, id);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(server);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (::sendto(s, reinterpret_cast<const char*>(q.data()), static_cast<int>(q.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) break;
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        timeval tv{2, 0};
        if (::select(static_cast<int>(s) + 1, &r, nullptr, nullptr, &tv) > 0) {
            uint8_t buf[1500];
            const int got = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (got > 12 && ((static_cast<unsigned>(buf[0]) << 8) | buf[1]) == id) ok = parseFirstARecord(buf, static_cast<std::size_t>(got), ip);
            break;                                   // an answer (good or not) ends the wait
        }
    }
    closeSocket(s);
    return ok;
}

// One worker thread, started on the first lookup and never torn down (so nothing is destroyed under it at exit).
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool started = false;

    void run() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [this] { return !jobs.empty(); });
                j = std::move(jobs.front()); jobs.pop_front();
            }
            bool ok = false; uint32_t ip = 0;
            if (j.upstream != 0) ok = lookupViaServer(j.name, j.upstream, ip);
            else {
                addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
                addrinfo* res = nullptr;
                if (getaddrinfo(j.name.c_str(), nullptr, &hints, &res) == 0 && res) {
                    for (addrinfo* a = res; a; a = a->ai_next)
                        if (a->ai_family == AF_INET) { ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr); ok = true; break; }
                    freeaddrinfo(res);
                }
            }
            if (j.done) j.done(ok, ip);
        }
    }
};

Worker& worker() { static Worker* w = new Worker(); return *w; }

} // namespace

void resolveHostAsync(const std::string& name, std::function<void(bool, uint32_t)> done, uint32_t upstream) {
    Worker& w = worker();
    {
        std::lock_guard<std::mutex> lock(w.m);
        w.jobs.push_back({name, std::move(done), upstream});
        if (!w.started) { w.started = true; std::thread([&w] { w.run(); }).detach(); }
    }
    w.cv.notify_one();
}

} // namespace beta::net
