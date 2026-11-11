#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace beta::net {

// Looks a host name up. By default with the host's own resolver; with `upstream` (an IPv4 address, host byte order) by asking that DNS server directly over UDP (for example
// 1.1.1.1). The lookup runs on a worker thread (it can take seconds when the network is slow or down) and `done` is called from that thread with the first IPv4 address
// (host byte order) or ok = false.
void resolveHostAsync(const std::string& name, std::function<void(bool ok, uint32_t ipv4)> done, uint32_t upstream = 0);

// The wire format used for the upstream lookup, public for the tests: a standard query for the A record of `name`, and the first A record (following CNAME chains, as the answer
// section lists them in order) of a response.
std::vector<uint8_t> buildDnsQuery(const std::string& name, uint16_t id);
bool parseFirstARecord(const uint8_t* msg, std::size_t n, uint32_t& ipv4);

} // namespace beta::net
