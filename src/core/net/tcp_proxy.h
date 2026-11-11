#pragma once

// The TCP layer of the virtual access point: terminates the console's TCP connections and relays them over host connections (net/host_tcp.h for the real ones).
// The virtual link is lossless and in order, so this is a small TCP: no retransmission timers, but real sequence numbers, checksums, the console's advertised window,
// and a proper close. Pure logic: the host side is injected (`connector`), so it is unit-tested with a fake host (tests/test_tcp_proxy.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace beta::net {

class TcpProxy {
public:
    using Frame = std::vector<uint8_t>;

    // What a host connection reports back (any thread).
    struct HostEvents {
        std::function<void(bool ok)> connected;
        std::function<void(const uint8_t* data, std::size_t n)> data;
        std::function<void()> closed;
    };
    class HostConn {
    public:
        virtual ~HostConn() {}
        virtual void send(const uint8_t* data, std::size_t n) = 0;
        virtual void shutdownWrite() = 0;
        virtual void close() = 0;
    };
    using Connector = std::function<std::shared_ptr<HostConn>(uint32_t ip, uint16_t port, HostEvents events)>;

    Connector connector;                         // opens a host connection (null: every connection is refused)
    std::function<void(Frame)> emit;             // a frame for the console
    std::function<void(const std::string&)> log;

    static constexpr std::size_t kMss = 1380;    // payload bytes per segment
    static constexpr uint32_t kMaxInFlight = 16384;

    // `f`: an Ethernet frame holding an IPv4 TCP segment from the console.
    void fromConsole(const uint8_t* f, std::size_t n) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (n < 14 + 20 + 20) return;
        const uint8_t* ip = f + 14;
        const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0xF) * 4;
        const std::size_t total = be16(ip + 2);
        if (ihl < 20 || total < ihl + 20 || 14 + total > n) return;
        const uint8_t* t = ip + ihl;
        const std::size_t doff = static_cast<std::size_t>(t[12] >> 4) * 4;
        if (doff < 20 || ihl + doff > total) return;
        const uint8_t flags = t[13];
        const uint32_t seq = be32(t + 4), ack = be32(t + 8);
        const unsigned window = be16(t + 14);
        const uint8_t* payload = t + doff;
        const std::size_t plen = total - ihl - doff;
        const Key key{be32(ip + 12), be16(t), be32(ip + 16), be16(t + 2)};
        auto it = conns_.find(key);

        if (flags & 0x04) {                                     // RST from the console: drop the connection
            if (it != conns_.end()) { if (it->second.host) it->second.host->close(); conns_.erase(it); }
            return;
        }
        if ((flags & 0x02) && !(flags & 0x10)) {                // SYN: a new connection
            if (it != conns_.end()) return;                     // a repeated SYN: the answer is on its way
            if (!allowed(key.dip)) { Conn c = fresh(key, f + 6, seq); sendSeg(c, 0x14, 0, c.rcvNxt, nullptr, 0, false); return; }
            Conn c = fresh(key, f + 6, seq);
            c.cwnd = window;
            auto ins = conns_.emplace(key, std::move(c)).first;
            HostEvents ev;
            ev.connected = [this, key](bool ok) { onHostConnected(key, ok); };
            ev.data = [this, key](const uint8_t* d, std::size_t k) { onHostData(key, d, k); };
            ev.closed = [this, key] { onHostClosed(key); };
            say("tcp connect " + ipText(key.dip) + ":" + std::to_string(key.dport));
            std::shared_ptr<HostConn> h = connector ? connector(key.dip, static_cast<uint16_t>(key.dport), ev) : nullptr;
            auto again = conns_.find(key);                      // the connector may have answered (and removed the entry) already
            if (again == conns_.end()) return;
            if (!h) { sendSeg(again->second, 0x14, 0, again->second.rcvNxt, nullptr, 0, false); conns_.erase(again); return; }
            again->second.host = h;
            (void)ins;
            return;
        }
        if (it == conns_.end()) {                               // a segment for a connection that does not exist: reset it
            Conn c = fresh(key, f + 6, seq);
            if (flags & 0x10) sendSeg(c, 0x04, ack, 0, nullptr, 0, false);
            else sendSeg(c, 0x14, 0, seq + static_cast<uint32_t>(plen) + ((flags & 0x01) ? 1u : 0u), nullptr, 0, false);
            return;
        }
        Conn& c = it->second;
        c.cwnd = window;
        if (flags & 0x10) {
            if (static_cast<int32_t>(ack - c.sndUna) > 0 && static_cast<int32_t>(c.sndNxt - ack) >= 0) c.sndUna = ack;
            if (c.finSent && c.sndUna == c.sndNxt && c.consoleFin) { if (c.host) c.host->close(); conns_.erase(it); return; }
        }
        if (c.state == State::Established) {
            bool needAck = false;
            if (plen > 0) {
                if (seq == c.rcvNxt) { if (c.host) c.host->send(payload, plen); c.rcvNxt += static_cast<uint32_t>(plen); }
                needAck = true;                                 // new data, a retransmission or a gap: say what is expected
            }
            if ((flags & 0x01) && seq + static_cast<uint32_t>(plen) == c.rcvNxt) {   // FIN in sequence
                c.rcvNxt += 1; c.consoleFin = true; needAck = true;
                if (c.host) c.host->shutdownWrite();
            }
            if (needAck) sendSeg(c, 0x10, c.sndNxt, c.rcvNxt, nullptr, 0, false);
            pump(c);
            if (c.finSent && c.sndUna == c.sndNxt && c.consoleFin) { if (c.host) c.host->close(); conns_.erase(key); }
        }
    }

    std::size_t connections() const { std::lock_guard<std::recursive_mutex> lock(mu_); return conns_.size(); }

private:
    enum class State { SynRcvd, Established };
    struct Key {
        uint32_t cip; unsigned cport; uint32_t dip; unsigned dport;
        bool operator<(const Key& o) const {
            if (cip != o.cip) return cip < o.cip;
            if (cport != o.cport) return cport < o.cport;
            if (dip != o.dip) return dip < o.dip;
            return dport < o.dport;
        }
    };
    struct Conn {
        Key key{}; uint8_t cmac[6] = {};
        State state = State::SynRcvd;
        uint32_t isn = 0, sndNxt = 0, sndUna = 0, rcvNxt = 0;
        unsigned cwnd = 0;
        std::deque<uint8_t> pending;                // from the host, not yet sent to the console
        bool hostClosed = false, finSent = false, consoleFin = false;
        std::shared_ptr<HostConn> host;
    };

    mutable std::recursive_mutex mu_;
    std::map<Key, Conn> conns_;
    uint32_t nextIsn_ = 0x1A2B3C00u;
    uint16_t ipId_ = 1;

    static unsigned be16(const uint8_t* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
    static uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
    static void put16(uint8_t* p, unsigned v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
    static void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }
    static uint32_t sum16(const uint8_t* p, std::size_t n, uint32_t acc) {
        for (std::size_t i = 0; i + 1 < n; i += 2) acc += be16(p + i);
        if (n & 1) acc += static_cast<uint32_t>(p[n - 1]) << 8;
        return acc;
    }
    static unsigned fold(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return static_cast<unsigned>(~s & 0xFFFF); }
    static std::string ipText(uint32_t ip) { return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255); }
    void say(const std::string& s) { if (log) log(s); }

    // Addresses the console may reach: the internet and the local network, not loopback, "this network", link-local, multicast or broadcast.
    static bool allowed(uint32_t ip) {
        const unsigned a = ip >> 24;
        if (a == 0 || a == 127 || a >= 224) return false;
        if (a == 169 && ((ip >> 16) & 255) == 254) return false;
        return true;
    }

    Conn fresh(const Key& key, const uint8_t* mac, uint32_t clientSeq) {
        Conn c; c.key = key; std::memcpy(c.cmac, mac, 6);
        c.isn = nextIsn_; nextIsn_ += 0x10000;
        c.sndNxt = c.sndUna = c.isn;
        c.rcvNxt = clientSeq + 1;
        return c;
    }

    // One segment from the server side to the console.
    void sendSeg(const Conn& c, uint8_t flags, uint32_t seq, uint32_t ack, const uint8_t* data, std::size_t len, bool mss) {
        const std::size_t opt = mss ? 4 : 0, tcpLen = 20 + opt + len;
        Frame r(14 + 20 + tcpLen, 0);
        std::memcpy(&r[0], c.cmac, 6);
        static const uint8_t kApMac[6] = {0x02, 0x50, 0x53, 0x50, 0x4C, 0x01};
        std::memcpy(&r[6], kApMac, 6);
        put16(&r[12], 0x0800);
        uint8_t* ip = &r[14];
        ip[0] = 0x45; put16(ip + 2, static_cast<unsigned>(20 + tcpLen)); put16(ip + 4, ipId_++); put16(ip + 6, 0x4000); ip[8] = 64; ip[9] = 6;
        put32(ip + 12, c.key.dip); put32(ip + 16, c.key.cip);
        put16(ip + 10, fold(sum16(ip, 20, 0)));
        uint8_t* t = ip + 20;
        put16(t, c.key.dport); put16(t + 2, c.key.cport); put32(t + 4, seq); put32(t + 8, ack);
        t[12] = static_cast<uint8_t>(((5 + (mss ? 1 : 0)) << 4)); t[13] = flags; put16(t + 14, 8192);
        if (mss) { t[20] = 2; t[21] = 4; put16(t + 22, static_cast<unsigned>(kMss)); }
        if (len) std::memcpy(t + 20 + opt, data, len);
        uint8_t pseudo[12]; put32(pseudo, c.key.dip); put32(pseudo + 4, c.key.cip); pseudo[8] = 0; pseudo[9] = 6; put16(pseudo + 10, static_cast<unsigned>(tcpLen));
        put16(t + 16, fold(sum16(t, tcpLen, sum16(pseudo, 12, 0))));
        if (emit) emit(std::move(r));
    }

    // Sends what the host has delivered as far as the console's window allows, then the FIN once the host is done.
    void pump(Conn& c) {
        if (c.state != State::Established) return;
        for (;;) {
            const uint32_t inflight = c.sndNxt - c.sndUna;
            const uint32_t room = c.cwnd < kMaxInFlight ? c.cwnd : kMaxInFlight;
            if (c.pending.empty() || inflight >= room) break;
            std::size_t n = c.pending.size() < kMss ? c.pending.size() : kMss;
            if (n > room - inflight) n = room - inflight;
            std::vector<uint8_t> chunk(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(n));
            c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(n));
            sendSeg(c, 0x18, c.sndNxt, c.rcvNxt, chunk.data(), n, false);
            c.sndNxt += static_cast<uint32_t>(n);
        }
        if (c.pending.empty() && c.hostClosed && !c.finSent) {
            sendSeg(c, 0x11, c.sndNxt, c.rcvNxt, nullptr, 0, false);
            c.sndNxt += 1; c.finSent = true;
        }
    }

    void onHostConnected(const Key& key, bool ok) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        auto it = conns_.find(key);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        if (!ok) { say("tcp connect failed"); sendSeg(c, 0x14, 0, c.rcvNxt, nullptr, 0, false); conns_.erase(it); return; }
        c.state = State::Established;
        sendSeg(c, 0x12, c.isn, c.rcvNxt, nullptr, 0, true);   // SYN|ACK
        c.sndNxt = c.isn + 1; c.sndUna = c.isn;
        pump(c);
    }
    void onHostData(const Key& key, const uint8_t* d, std::size_t n) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        auto it = conns_.find(key);
        if (it == conns_.end()) return;
        it->second.pending.insert(it->second.pending.end(), d, d + n);
        pump(it->second);
    }
    void onHostClosed(const Key& key) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        auto it = conns_.find(key);
        if (it == conns_.end()) return;
        it->second.hostClosed = true;
        pump(it->second);
    }
};

} // namespace beta::net
