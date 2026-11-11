#pragma once

// The virtual access point the emulated Wi-Fi chip is connected to: the network the console joins ("PSP-LLE", open). It answers the console's Ethernet frames the way a small router would.
// Layer 1: ARP for the gateway and DHCP, which give the console an address and tell it the gateway and DNS server.
// Layer 2: DNS, forwarded to the host's resolver (the lookup is asynchronous: the answer arrives later).
// Later layers (ICMP, TCP and UDP forwarded to the host's sockets) sit on top of the same frame interface: fromConsole() takes a frame, replies are queued for the console.
// The logic has no I/O of its own (the resolver is injected), so it is unit-tested with synthetic frames (tests/test_virtual_ap.cpp). The reply queue is locked: a resolver
// may answer from another thread.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tcp_proxy.h"
#include "udp_proxy.h"

namespace beta::net {

class VirtualAp {
public:
    using Frame = std::vector<uint8_t>;
    using ResolveDone = std::function<void(bool ok, uint32_t ipv4)>;

    static constexpr uint8_t kMac[6] = {0x02, 0x50, 0x53, 0x50, 0x4C, 0x01};   // the BSSID and the gateway's hardware address
    static constexpr uint32_t kGatewayIp = (192u << 24) | (168u << 16) | (77u << 8) | 1u;
    static constexpr uint32_t kClientIp = (192u << 24) | (168u << 16) | (77u << 8) | 2u;
    static constexpr uint32_t kNetmask = 0xFFFFFF00u;

    // Set by the owner: how a host name is looked up (calls `done` once, possibly from another thread), and where to report what happens (may be empty).
    std::function<void(const std::string& name, ResolveDone done)> resolver;
    std::function<void(const std::string& line)> log;

    VirtualAp() {
        tcp_.emit = [this](Frame f) { push(std::move(f)); };
        tcp_.log = [this](const std::string& s) { say(s); };
        udp_.emit = [this](Frame f) { push(std::move(f)); };
        udp_.log = [this](const std::string& s) { say(s); };
    }
    UdpProxy& udp() { return udp_; }   // its `opener` decides how UDP datagrams reach the host (none: dropped)
    TcpProxy& tcp() { return tcp_; }   // its `connector` decides how TCP connections reach the host (none: all refused)

    void fromConsole(const uint8_t* f, std::size_t n) {
        if (n < 14) return;
        const unsigned type = be16(f + 12);
        if (type == 0x0806) handleArp(f, n);
        else if (type == 0x0800) handleIpv4(f, n);
    }

    // The frames for the console, oldest first.
    bool pop(Frame& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (outbox_.empty()) return false;
        out = std::move(outbox_.front()); outbox_.pop_front();
        return true;
    }
    bool empty() const { std::lock_guard<std::mutex> lock(mu_); return outbox_.empty(); }
    void clear() { std::lock_guard<std::mutex> lock(mu_); outbox_.clear(); }
    std::size_t pending() const { std::lock_guard<std::mutex> lock(mu_); return outbox_.size(); }

    // ---- helpers shared with the tests -------------------------------------------------------------------------------------------------------------------
    static unsigned be16(const uint8_t* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
    static uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
    static void put16(uint8_t* p, unsigned v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
    static void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }
    // The Internet checksum (RFC 1071) of `n` bytes.
    static unsigned checksum(const uint8_t* p, std::size_t n) {
        uint32_t sum = 0;
        for (std::size_t i = 0; i + 1 < n; i += 2) sum += be16(p + i);
        if (n & 1) sum += static_cast<uint32_t>(p[n - 1]) << 8;
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        return static_cast<unsigned>(~sum & 0xFFFF);
    }

private:
    mutable std::mutex mu_;
    std::deque<Frame> outbox_;
    TcpProxy tcp_;
    UdpProxy udp_;
    void push(Frame f) { std::lock_guard<std::mutex> lock(mu_); outbox_.push_back(std::move(f)); }
    void say(const std::string& s) { if (log) log(s); }

    void handleArp(const uint8_t* f, std::size_t n) {
        if (n < 42) return;
        const uint8_t* a = f + 14;
        if (be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
        if (be16(a + 6) != 1) return;                 // a request
        if (be32(a + 24) != kGatewayIp) return;       // asking for the gateway
        Frame r(42, 0);
        std::memcpy(&r[0], a + 8, 6);                 // to the asker
        std::memcpy(&r[6], kMac, 6);
        put16(&r[12], 0x0806);
        put16(&r[14], 1); put16(&r[16], 0x0800); r[18] = 6; r[19] = 4; put16(&r[20], 2);
        std::memcpy(&r[22], kMac, 6); put32(&r[28], kGatewayIp);          // sender: the gateway
        std::memcpy(&r[32], a + 8, 6); std::memcpy(&r[38], a + 14, 4);    // target: the asker
        push(std::move(r));
    }

    void handleIpv4(const uint8_t* f, std::size_t n) {
        if (n < 14 + 20) return;
        const uint8_t* ip = f + 14;
        if ((ip[0] >> 4) != 4) return;
        const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0xF) * 4;
        if (ihl < 20 || 14 + ihl + 8 > n) return;
        if (ip[9] == 6) { tcp_.fromConsole(f, n); return; }   // TCP
        if (ip[9] == 17) {                            // UDP
            const uint8_t* u = ip + ihl;
            const std::size_t payload = n - (14 + ihl + 8);
            const unsigned dport = be16(u + 2);
            if (dport == 67) handleDhcp(u + 8, payload);
            else if (dport == 53 && be32(ip + 16) == kGatewayIp) handleDns(f, ip, ihl, u, payload);
            else udp_.fromConsole(f, n);
        }
    }

    // A UDP datagram from the gateway to the console that sent `ethReq`/`ipReq` (the request's addresses and ports swapped).
    Frame udpReply(const uint8_t* ethReq, const uint8_t* ipReq, std::size_t ihl, const uint8_t* payload, std::size_t len) {
        const uint8_t* u = ipReq + ihl;
        Frame r(14 + 20 + 8 + len, 0);
        std::memcpy(&r[0], ethReq + 6, 6);            // to the console's hardware address
        std::memcpy(&r[6], kMac, 6);
        put16(&r[12], 0x0800);
        uint8_t* ip = &r[14];
        ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
        std::memcpy(ip + 12, ipReq + 16, 4);          // from the address the console asked
        std::memcpy(ip + 16, ipReq + 12, 4);          // to the console
        put16(ip + 2, static_cast<unsigned>(20 + 8 + len));
        put16(ip + 10, checksum(ip, 20));
        uint8_t* ur = ip + 20;
        std::memcpy(ur, u + 2, 2); std::memcpy(ur + 2, u, 2);   // ports swapped
        put16(ur + 4, static_cast<unsigned>(8 + len));          // the UDP checksum stays 0 (allowed over IPv4)
        if (len) std::memcpy(ur + 8, payload, len);
        return r;
    }

    // DNS (RFC 1035): a standard query for one name. A records are looked up; other types get an empty answer; a failed lookup is NXDOMAIN.
    void handleDns(const uint8_t* eth, const uint8_t* ip, std::size_t ihl, const uint8_t* u, std::size_t len) {
        const uint8_t* q = u + 8;
        if (len < 12 || (q[2] & 0x80) || ((q[2] >> 3) & 0xF) != 0 || be16(q + 4) != 1) return;   // a query, opcode 0, one question
        std::string name; std::size_t i = 12;
        while (i < len && q[i] != 0) {
            const std::size_t l = q[i];
            if (l > 63 || i + 1 + l >= len) return;
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char*>(q + i + 1), l);
            i += 1 + l;
        }
        if (i + 5 > len) return;
        const unsigned qtype = be16(q + i + 1), qclass = be16(q + i + 3);
        const std::size_t qend = i + 5;
        say("dns query " + name + " type " + std::to_string(qtype));
        // the pieces needed to answer later (the request may be gone by then)
        const Frame ethCopy(eth, eth + 14), ipCopy(ip, ip + ihl + 8);
        const Frame question(q + 12, q + qend);
        const unsigned id = be16(q), flagsRd = q[2] & 1;
        auto answer = [this, ethCopy, ipCopy, ihl, question, id, flagsRd, name](int rcode, bool haveA, uint32_t addr) {
            Frame d(12 + question.size() + (haveA ? 16 : 0), 0);
            put16(&d[0], id);
            put16(&d[2], 0x8000 | (flagsRd << 8) | 0x80 | static_cast<unsigned>(rcode));   // response, recursion desired echoed, recursion available
            put16(&d[4], 1); put16(&d[6], haveA ? 1 : 0);
            std::memcpy(&d[12], question.data(), question.size());
            if (haveA) {
                uint8_t* a = &d[12 + question.size()];
                put16(a, 0xC00C); put16(a + 2, 1); put16(a + 4, 1); put32(a + 6, 60); put16(a + 10, 4); put32(a + 12, addr);
            }
            push(udpReply(ethCopy.data(), ipCopy.data(), ihl, d.data(), d.size()));
            say("dns answer " + name + (haveA ? "" : rcode ? " (no such name)" : " (no data)"));
        };
        if (qclass != 1) { answer(4, false, 0); return; }                               // not implemented
        if (qtype != 1) { answer(0, false, 0); return; }                                // no A record asked: an empty, successful answer
        if (!resolver) { answer(2, false, 0); return; }                                 // server failure
        resolver(name, [answer](bool ok, uint32_t ipv4) { if (ok) answer(0, true, ipv4); else answer(3, false, 0); });
    }

    // DHCP (RFC 2131): DISCOVER -> OFFER, REQUEST -> ACK, always the same address for the one console.
    void handleDhcp(const uint8_t* b, std::size_t len) {
        if (len < 241 || b[0] != 1 || be32(b + 236) != 0x63825363u) return;   // BOOTREQUEST with the magic cookie
        int msgType = 0;
        for (std::size_t i = 240; i < len;) {
            const unsigned code = b[i];
            if (code == 255) break;
            if (code == 0) { ++i; continue; }
            if (i + 1 >= len) break;
            const std::size_t olen = b[i + 1];
            if (i + 2 + olen > len) break;
            if (code == 53 && olen >= 1) msgType = b[i + 2];
            i += 2 + olen;
        }
        if (msgType != 1 && msgType != 3) return;
        const uint8_t reply = msgType == 1 ? 2 : 5;   // OFFER / ACK

        Frame r(14 + 20 + 8 + 240 + 40, 0);
        uint8_t* e = &r[0];
        std::memset(e, 0xFF, 6);                      // broadcast: the console has no address yet
        std::memcpy(e + 6, kMac, 6);
        put16(e + 12, 0x0800);
        uint8_t* ip = e + 14;
        ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
        put32(ip + 12, kGatewayIp); put32(ip + 16, 0xFFFFFFFFu);
        uint8_t* u = ip + 20;
        put16(u, 67); put16(u + 2, 68);
        uint8_t* d = u + 8;
        d[0] = 2; d[1] = 1; d[2] = 6;
        std::memcpy(d + 4, b + 4, 4);                 // xid
        std::memcpy(d + 10, b + 10, 2);               // flags
        put32(d + 16, kClientIp);                     // yiaddr
        put32(d + 20, kGatewayIp);                    // siaddr
        std::memcpy(d + 28, b + 28, 16);              // chaddr
        put32(d + 236, 0x63825363u);
        std::size_t o = 240;
        auto opt = [&](uint8_t code, uint8_t olen, const uint8_t* data) { d[o++] = code; d[o++] = olen; for (uint8_t i = 0; i < olen; ++i) d[o++] = data[i]; };
        uint8_t t = reply; opt(53, 1, &t);
        uint8_t v[4];
        put32(v, kGatewayIp); opt(54, 4, v);                     // server identifier
        put32(v, 86400); opt(51, 4, v);                          // lease time: a day
        put32(v, kNetmask); opt(1, 4, v);                        // subnet mask
        put32(v, kGatewayIp); opt(3, 4, v);                      // router
        put32(v, kGatewayIp); opt(6, 4, v);                      // DNS server: the gateway forwards
        d[o++] = 255;
        const std::size_t udpLen = 8 + o, ipLen = 20 + udpLen;
        put16(u + 4, static_cast<unsigned>(udpLen));             // UDP checksum stays 0 (allowed over IPv4)
        put16(ip + 2, static_cast<unsigned>(ipLen));
        put16(ip + 10, checksum(ip, 20));
        r.resize(14 + ipLen);
        push(std::move(r));
    }
};

} // namespace beta::net
