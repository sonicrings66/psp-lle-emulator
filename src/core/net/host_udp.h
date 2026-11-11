#pragma once

#include <cstdint>
#include <memory>

#include "udp_proxy.h"

namespace beta::net {

// A host UDP socket "connected" to ip:port (host byte order), for the virtual access point's UDP proxy: datagrams from that address come back through `onData` (from a thread
// of its own), send() sends one, close() ends it.
std::shared_ptr<UdpProxy::HostSock> hostUdpOpen(uint32_t ip, uint16_t port, UdpProxy::OnData onData);

} // namespace beta::net
