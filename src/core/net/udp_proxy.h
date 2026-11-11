#pragma once

// The UDP layer of the virtual access point: every UDP datagram the console sends to an address outside the virtual network (and that is not DHCP or a DNS query for the gateway)
// goes out through a host UDP socket, and what comes back is delivered to the console as a datagram from that address. One host socket per (console port, remote address, remote
// port), kept while it is used (at most 64 at a time: the oldest is closed first). Pure logic with the host side injected (`opener`), so it is unit-tested with a fake host.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace beta::net {

class UdpProxy {
public:
    using Frame = std::vector<uint8_t>;
    using OnData = std::function<void(const uint8_t* data, std::size_t n)>;
    class HostSock {
    public:
        virtual ~HostSock() {}
        virtual void send(const uint8_t* data, std::size_t n) = 0;
        virtual void close() = 0;
    };
    using Opener = std::function<std::shared_ptr<HostSock>(uint32_t ip, uint16_t port, OnData onData)>;

    Opener opener;                                // opens a host socket to ip:port (null: nothing is forwarded)
    std::function<void(Frame)> emit;              // a frame for the console
    std::function<void(const std::string&)> log;

    static constexpr std::size_t kMaxSockets = 64;
    static constexpr std::size_t kMaxPayload = 1472;

    // `f`: an Ethernet frame holding an IPv4 UDP datagram from the console.
    void fromConsole(const uint8_t* f, std::size_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        if (n < 14 + 20 + 8) return;
        const uint8_t* ip = f + 14;
        const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0xF) * 4;
        const std::size_t total = be16(ip + 2);
        if (ihl < 20 || total < ihl + 8 || 14 + total > n) return;
        const uint8_t* u = ip + ihl;
        const std::size_t ulen = be16(u + 4);
        if (ulen < 8 || ulen > total - ihl) return;
        const std::size_t plen = ulen - 8;
        const Key key{be32(ip + 12), be16(u), be32(ip + 16), be16(u + 2)};
        if (!allowed(key.dip) || !opener) return;
        auto it = socks_.find(key);
        if (it == socks_.end()) {
            if (socks_.size() >= kMaxSockets) dropOldest();
            Entry e; std::memcpy(e.cmac, f + 6, 6); e.key = key; e.used = ++clock_;
            OnData cb = [this, key](const uint8_t* d, std::size_t k) { onHostData(key, d, k); };
            say("udp " + ipText(key.dip) + ":" + std::to_string(key.dport) + " from console port " + std::to_string(key.cport));
            e.sock = opener(key.dip, static_cast<uint16_t>(key.dport), cb);
            if (!e.sock) return;
            it = socks_.emplace(key, std::move(e)).first;
        }
        it->second.used = ++clock_;
        it->second.sock->send(u + 8, plen);
    }

    std::size_t sockets() const { std::lock_guard<std::mutex> lock(mu_); return socks_.size(); }
    void closeAll() { std::lock_guard<std::mutex> lock(mu_); for (auto& kv : socks_) if (kv.second.sock) kv.second.sock->close(); socks_.clear(); }

private:
    struct Key {
        uint32_t cip; unsigned cport; uint32_t dip; unsigned dport;
        bool operator<(const Key& o) const {
            if (cip != o.cip) return cip < o.cip;
            if (cport != o.cport) return cport < o.cport;
            if (dip != o.dip) return dip < o.dip;
            return dport < o.dport;
        }
    };
    struct Entry { Key key{}; uint8_t cmac[6] = {}; std::shared_ptr<HostSock> sock; uint64_t used = 0; };

    mutable std::mutex mu_;
    std::map<Key, Entry> socks_;
    uint64_t clock_ = 0;
    uint16_t ipId_ = 0x4000;

    static unsigned be16(const uint8_t* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
    static uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
    static void put16(uint8_t* p, unsigned v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
    static void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }
    static std::string ipText(uint32_t ip) { return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255); }
    void say(const std::string& s) { if (log) log(s); }

    // The same rule as the TCP proxy: the internet and the local network, not loopback, "this network", link-local, multicast or broadcast (so UPnP discovery goes unanswered).
    static bool allowed(uint32_t ip) {
        const unsigned a = ip >> 24;
        if (a == 0 || a == 127 || a >= 224) return false;
        if (a == 169 && ((ip >> 16) & 255) == 254) return false;
        return true;
    }

    void dropOldest() {
        auto oldest = socks_.begin();
        for (auto it = socks_.begin(); it != socks_.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
        if (oldest != socks_.end()) { if (oldest->second.sock) oldest->second.sock->close(); socks_.erase(oldest); }
    }

    void onHostData(const Key& key, const uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = socks_.find(key);
        if (it == socks_.end() || n > kMaxPayload || !emit) return;
        Frame r(14 + 20 + 8 + n, 0);
        std::memcpy(&r[0], it->second.cmac, 6);
        static const uint8_t kApMac[6] = {0x02, 0x50, 0x53, 0x50, 0x4C, 0x01};
        std::memcpy(&r[6], kApMac, 6);
        put16(&r[12], 0x0800);
        uint8_t* ip = &r[14];
        ip[0] = 0x45; put16(ip + 2, static_cast<unsigned>(20 + 8 + n)); put16(ip + 4, ipId_++); put16(ip + 6, 0x4000); ip[8] = 64; ip[9] = 17;
        put32(ip + 12, key.dip); put32(ip + 16, key.cip);
        uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += be16(ip + i);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        put16(ip + 10, static_cast<unsigned>(~sum & 0xFFFF));
        uint8_t* u = ip + 20;
        put16(u, key.dport); put16(u + 2, key.cport); put16(u + 4, static_cast<unsigned>(8 + n));   // the UDP checksum stays 0 (allowed over IPv4)
        if (n) std::memcpy(u + 8, d, n);
        emit(std::move(r));
    }
};

} // namespace beta::net
