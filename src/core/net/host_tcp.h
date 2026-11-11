#pragma once

#include <cstdint>
#include <memory>

#include "tcp_proxy.h"

namespace beta::net {

// A real TCP connection from the host to ip:port (host byte order), for the virtual access point's TCP proxy. The connect, and the reads that follow, run on a thread of their
// own and report through `events`; the returned object sends, half-closes and closes. The connect gives up after 8 seconds.
std::shared_ptr<TcpProxy::HostConn> hostTcpConnect(uint32_t ip, uint16_t port, TcpProxy::HostEvents events);

} // namespace beta::net
