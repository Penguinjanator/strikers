// Datagram transport for online play: addresses, the pipe, and the system's sockets and entropy.

#ifndef PORT_ONLINE_PIPE_H
#define PORT_ONLINE_PIPE_H

#include <cstddef>
#include <cstdint>

namespace online
{
struct NetAddr
{
    uint8_t family;   // 0 for none, 4 or 6
    uint8_t ip[16];   // an IPv4 address fills the first four bytes
    uint16_t port;
};

bool AddrEqual(const NetAddr& a, const NetAddr& b);

// "a.b.c.d:port" or "[v6]:port".
void AddrFormat(const NetAddr& a, char* out, size_t size);

// Loopback, private (10/8, 172.16/12, 192.168/16, 100.64/10) or link-local.
bool AddrIsLanSource(const NetAddr& a);

// Splits "host", "host:port", "[v6]:port" or a bare IPv6 address; false when malformed.
bool AddrSplit(const char* text, char* host, size_t hostSize, uint16_t* port, uint16_t defaultPort);

// A numeric IPv4 or IPv6 address with no lookup.
bool AddrParseNumeric(const char* host, uint16_t port, NetAddr* out);

class NetPipe
{
  public:
    virtual ~NetPipe() {}
    virtual int Send(const NetAddr& to, const uint8_t* data, size_t size) = 0;   // 0 on success
    virtual int Receive(NetAddr* from, uint8_t* data, size_t cap) = 0;           // bytes, or -1
    // The UDP pipe whose socket UdpWaitAny and UdpLocalPort use, or NULL.
    virtual NetPipe* SocketPipe() { return NULL; }
};

enum
{
    PIPE_OK = 0,
    PIPE_ERR_NETWORK,
    PIPE_ERR_IN_USE
};

// Winsock or the Switch socket driver, once; false when the platform has no network stack.
bool NetStartup(void);
void NetShutdown(void);

// A non-blocking UDP socket, dual stack for family 6; `shareable` lets processes share it.
NetPipe* UdpOpen(int family, uint16_t port, bool shareable, bool broadcast, int* error);
void UdpClose(NetPipe* pipe);
uint16_t UdpLocalPort(NetPipe* pipe);

// Returns when any pipe is readable or `timeoutUs` has passed.
void UdpWaitAny(NetPipe* const* pipes, int count, uint64_t timeoutUs);

// A blocking name lookup; IPv4 only on Switch.
bool NetResolve(const char* host, uint16_t port, NetAddr* out);

// 255.255.255.255 and each IPv4 interface's directed broadcast, at `port`.
int NetBroadcastTargets(NetAddr* out, int max, uint16_t port);

// The best private IPv4 address of this machine, as text.
bool NetLanAddress(char* out, size_t size);

// Every IPv4 address of this machine but loopback and link-local, best first as NetLanAddress
// ranks them: a VPN's (Tailscale's 100.64/10, ZeroTier's, Hamachi's) comes after the LAN's.
int NetLanAddresses(char out[][16], int max);

bool NetRandom(void* buf, size_t size);
}   // namespace online

#endif   // PORT_ONLINE_PIPE_H
