// UDP for online play: POSIX, Winsock on Windows and UWP, libnx sockets on Switch (IPv4 only).

#include "pipe.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winapifamily.h>
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
#define ONLINE_UWP 1
#else
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "bcrypt")
#endif
typedef SOCKET PortSocket;
#define PORT_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__SWITCH__)
#include <switch.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/random.h>
#endif
typedef int PortSocket;
#define PORT_BAD_SOCKET (-1)
#endif

namespace online
{
namespace
{
#if defined(_WIN32)
WSAEVENT s_event = WSA_INVALID_EVENT;
HANDLE s_timer;

void close_socket(PortSocket s) { closesocket(s); }
bool in_use(void) { return WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == WSAEACCES; }
#else
void close_socket(PortSocket s) { close(s); }
bool in_use(void) { return errno == EADDRINUSE || errno == EACCES; }
#endif

#if defined(__SWITCH__)
bool s_ownDriver;
#endif
int s_started;

void to_sockaddr(const NetAddr& a, int socketFamily, sockaddr_storage* ss, socklen_t* len)
{
    std::memset(ss, 0, sizeof *ss);
#if !defined(__SWITCH__)
    if (socketFamily == 6)
    {
        sockaddr_in6* s6 = (sockaddr_in6*)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a.port);
        if (a.family == 4)
        {
            // A dual-stack socket reaches IPv4 peers through mapped addresses.
            s6->sin6_addr.s6_addr[10] = 0xFF;
            s6->sin6_addr.s6_addr[11] = 0xFF;
            std::memcpy(&s6->sin6_addr.s6_addr[12], a.ip, 4);
        }
        else
            std::memcpy(&s6->sin6_addr, a.ip, 16);
        *len = sizeof(sockaddr_in6);
        return;
    }
#else
    (void)socketFamily;
#endif
    sockaddr_in* s4 = (sockaddr_in*)ss;
    s4->sin_family = AF_INET;
    s4->sin_port = htons(a.port);
    std::memcpy(&s4->sin_addr, a.ip, 4);
    *len = sizeof(sockaddr_in);
}

bool from_sockaddr(const sockaddr* sa, NetAddr* out)
{
    std::memset(out, 0, sizeof *out);
    if (sa->sa_family == AF_INET)
    {
        const sockaddr_in* s4 = (const sockaddr_in*)sa;
        out->family = 4;
        std::memcpy(out->ip, &s4->sin_addr, 4);
        out->port = ntohs(s4->sin_port);
        return true;
    }
#if !defined(__SWITCH__)
    if (sa->sa_family == AF_INET6)
    {
        const sockaddr_in6* s6 = (const sockaddr_in6*)sa;
        const uint8_t* b = (const uint8_t*)&s6->sin6_addr;
        static const uint8_t kMapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (std::memcmp(b, kMapped, 12) == 0)
        {
            out->family = 4;
            std::memcpy(out->ip, b + 12, 4);
        }
        else
        {
            out->family = 6;
            std::memcpy(out->ip, b, 16);
        }
        out->port = ntohs(s6->sin6_port);
        return true;
    }
#endif
    return false;
}

bool set_nonblocking(PortSocket s)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

class UdpPipe : public NetPipe
{
  public:
    UdpPipe(PortSocket s, int family)
        : m_socket(s), m_family(family)
    {}
    ~UdpPipe() override { close_socket(m_socket); }

    int Send(const NetAddr& to, const uint8_t* data, size_t size) override
    {
        if (to.family == 6 && m_family != 6)
            return -1;
        sockaddr_storage ss;
        socklen_t len;
        to_sockaddr(to, m_family, &ss, &len);
        const long sent =
            (long)sendto(m_socket, (const char*)data, (int)size, 0, (const sockaddr*)&ss, len);
        return sent == (long)size ? 0 : -1;
    }

    int Receive(NetAddr* from, uint8_t* data, size_t cap) override
    {
        for (;;)
        {
            sockaddr_storage ss;
            socklen_t len = sizeof ss;
            const long n = (long)recvfrom(m_socket, (char*)data, (int)cap, 0, (sockaddr*)&ss, &len);
            if (n < 0)
            {
#if defined(_WIN32)
                // A port-unreachable reply to an earlier send, or a datagram too large.
                const int e = WSAGetLastError();
                if (e == WSAECONNRESET || e == WSAEMSGSIZE)
                    continue;
#endif
                return -1;
            }
            if (!from_sockaddr((const sockaddr*)&ss, from))
                continue;
            return (int)n;
        }
    }

    NetPipe* SocketPipe() override { return this; }

    PortSocket Socket() const { return m_socket; }
    int Family() const { return m_family; }

  private:
    PortSocket m_socket;
    int m_family;
};

PortSocket open_socket(int family, uint16_t port, bool shareable, bool broadcast, int* error)
{
    const int af = family == 6 ? AF_INET6 : AF_INET;
    PortSocket s = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PORT_BAD_SOCKET)
    {
        *error = PIPE_ERR_NETWORK;
        return PORT_BAD_SOCKET;
    }
    int on = 1;
    int off = 0;
    (void)off;
#if !defined(__SWITCH__)
    if (af == AF_INET6)
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
#endif
    if (shareable)
    {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    }
    if (broadcast)
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);

    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof ss);
    socklen_t len;
#if !defined(__SWITCH__)
    if (af == AF_INET6)
    {
        sockaddr_in6* s6 = (sockaddr_in6*)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        s6->sin6_addr = in6addr_any;
        len = sizeof *s6;
    }
    else
#endif
    {
        sockaddr_in* s4 = (sockaddr_in*)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        s4->sin_addr.s_addr = htonl(INADDR_ANY);
        len = sizeof *s4;
    }
    if (bind(s, (const sockaddr*)&ss, len) != 0)
    {
        *error = in_use() ? PIPE_ERR_IN_USE : PIPE_ERR_NETWORK;
        close_socket(s);
        return PORT_BAD_SOCKET;
    }
    if (!set_nonblocking(s))
    {
        *error = PIPE_ERR_NETWORK;
        close_socket(s);
        return PORT_BAD_SOCKET;
    }
#if defined(_WIN32)
    if (s_event != WSA_INVALID_EVENT)
        WSAEventSelect(s, s_event, FD_READ);
#endif
    *error = PIPE_OK;
    return s;
}

// 4 for 192.168/16, 3 for 10/8, 2 for 172.16/12, 1 for any other usable address, 0 for none.
int lan_rank(const uint8_t ip[4])
{
    if (ip[0] == 192 && ip[1] == 168)
        return 4;
    if (ip[0] == 10)
        return 3;
    if (ip[0] == 172 && (ip[1] & 0xF0) == 16)
        return 2;
    if (ip[0] != 127 && ip[0] != 0 && !(ip[0] == 169 && ip[1] == 254))
        return 1;
    return 0;
}

#if !defined(ONLINE_UWP) && !defined(__SWITCH__)
// The source address the OS picks for its default route; connecting a UDP socket sends nothing.
bool routed_ipv4(uint8_t out[4])
{
    PortSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PORT_BAD_SOCKET)
        return false;
    sockaddr_in to;
    std::memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    // 192.0.2.1 is a documentation address: anything off the LAN takes the default route.
    static const uint8_t kAway[4] = { 192, 0, 2, 1 };
    std::memcpy(&to.sin_addr, kAway, 4);
    sockaddr_in self;
    std::memset(&self, 0, sizeof self);
    socklen_t len = sizeof self;
    const bool ok = connect(s, (const sockaddr*)&to, (socklen_t)sizeof to) == 0 &&
                    getsockname(s, (sockaddr*)&self, &len) == 0 && self.sin_family == AF_INET;
    if (ok)
        std::memcpy(out, &self.sin_addr, 4);
    close_socket(s);
    return ok;
}
#endif

#if defined(_WIN32) && !defined(ONLINE_UWP)
// Every IPv4 address with its interface's flags; the buffer grows until the list fits.
std::vector<INTERFACE_INFO> interface_list(void)
{
    std::vector<INTERFACE_INFO> info;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return info;
    for (size_t n = 16; n <= 1024; n *= 2)
    {
        info.resize(n);
        DWORD bytes = 0;
        if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, info.data(),
                     (DWORD)(n * sizeof(INTERFACE_INFO)), &bytes, NULL, NULL) == 0)
        {
            info.resize(bytes / sizeof(INTERFACE_INFO));
            closesocket(s);
            return info;
        }
        if (WSAGetLastError() != WSAEFAULT)
            break;
    }
    info.clear();
    closesocket(s);
    return info;
}
#endif
}   // namespace

bool NetStartup(void)
{
    if (s_started++ > 0)
        return true;
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        s_started = 0;
        return false;
    }
    s_event = WSACreateEvent();
    s_timer = NULL;
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
    s_timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    if (s_timer == NULL)
        s_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
#elif defined(__SWITCH__)
    // Boot closes the driver unless nxlink holds it; a UDP-sized configuration costs little heap.
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.tcp_tx_buf_size = 0x1000;
    config.tcp_rx_buf_size = 0x1000;
    config.tcp_tx_buf_max_size = 0;
    config.tcp_rx_buf_max_size = 0;
    // A blocking poll holds a session, and every other BSD call waits for a free one.
    config.num_bsd_sessions = 4;
    // Fails when nxlink already holds the driver, which then serves online play as well.
    s_ownDriver = R_SUCCEEDED(socketInitialize(&config));
#endif
    return true;
}

void NetShutdown(void)
{
    if (s_started == 0 || --s_started > 0)
        return;
#if defined(_WIN32)
    if (s_event != WSA_INVALID_EVENT)
        WSACloseEvent(s_event);
    s_event = WSA_INVALID_EVENT;
    if (s_timer != NULL)
        CloseHandle(s_timer);
    s_timer = NULL;
    WSACleanup();
#elif defined(__SWITCH__)
    if (s_ownDriver)
        socketExit();
    s_ownDriver = false;
#endif
}

NetPipe* UdpOpen(int family, uint16_t port, bool shareable, bool broadcast, int* error)
{
    int err = PIPE_ERR_NETWORK;
#if defined(__SWITCH__)
    family = 4;
#endif
    PortSocket s = open_socket(family, port, shareable, broadcast, &err);
    if (s == PORT_BAD_SOCKET && family == 6 && err != PIPE_ERR_IN_USE)
    {
        // No IPv6 on this host.
        family = 4;
        s = open_socket(4, port, shareable, broadcast, &err);
    }
    if (error != NULL)
        *error = err;
    if (s == PORT_BAD_SOCKET)
        return NULL;
    return new UdpPipe(s, family == 6 ? 6 : 4);
}

void UdpClose(NetPipe* pipe) { delete pipe; }

uint16_t UdpLocalPort(NetPipe* pipe)
{
    pipe = pipe != NULL ? pipe->SocketPipe() : NULL;
    if (pipe == NULL)
        return 0;
    sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(static_cast<UdpPipe*>(pipe)->Socket(), (sockaddr*)&ss, &len) != 0)
        return 0;
    NetAddr a;
    return from_sockaddr((const sockaddr*)&ss, &a) ? a.port : 0;
}

void UdpWaitAny(NetPipe* const* pipes, int count, uint64_t timeoutUs)
{
#if defined(_WIN32)
    // Winsock's own timeouts follow the 15.6 ms system tick, too coarse for a 20 ms send schedule.
    (void)pipes;
    (void)count;
    if (timeoutUs == 0)
        return;
    HANDLE handles[2];
    DWORD n = 0;
    if (s_event != WSA_INVALID_EVENT)
        handles[n++] = s_event;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(timeoutUs * 10u);
    if (s_timer != NULL && SetWaitableTimerEx(s_timer, &due, 0, NULL, NULL, NULL, 0))
        handles[n++] = s_timer;
    if (n == 0)
    {
        Sleep((DWORD)(timeoutUs / 1000u));
        return;
    }
    WaitForMultipleObjectsEx(n, handles, FALSE, (DWORD)(timeoutUs / 1000u + 20u), FALSE);
    if (s_event != WSA_INVALID_EVENT)
        WSAResetEvent(s_event);
#else
    struct pollfd fds[8];
    int n = 0;
    for (int i = 0; i < count && n < 8; i++)
    {
        if (pipes[i] == NULL || pipes[i]->SocketPipe() == NULL)
            continue;
        fds[n].fd = static_cast<UdpPipe*>(pipes[i]->SocketPipe())->Socket();
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    const int ms = (int)((timeoutUs + 999u) / 1000u);
    if (n == 0)
    {
        usleep((useconds_t)timeoutUs);
        return;
    }
    poll(fds, (nfds_t)n, ms);
#endif
}

bool NetResolve(const char* host, uint16_t port, NetAddr* out)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
#if defined(__SWITCH__)
    hints.ai_family = AF_INET;
#else
    hints.ai_family = AF_UNSPEC;
#endif
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = NULL;
    char service[8];
    std::snprintf(service, sizeof service, "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &list) != 0 || list == NULL)
        return false;
    bool ok = false;
    // IPv4 first: every platform has it, and a host's dual-stack socket takes both.
    for (int pass = 0; pass < 2 && !ok; pass++)
    {
        for (addrinfo* ai = list; ai != NULL && !ok; ai = ai->ai_next)
        {
            if ((pass == 0) != (ai->ai_family == AF_INET))
                continue;
            ok = from_sockaddr(ai->ai_addr, out);
        }
    }
    freeaddrinfo(list);
    if (ok)
        out->port = port;
    return ok;
}

int NetBroadcastTargets(NetAddr* out, int max, uint16_t port)
{
    int n = 0;
    auto add = [&](const uint8_t ip[4]) {
        for (int i = 0; i < n; i++)
            if (std::memcmp(out[i].ip, ip, 4) == 0)
                return;
        if (n >= max)
            return;
        std::memset(&out[n], 0, sizeof out[n]);
        out[n].family = 4;
        std::memcpy(out[n].ip, ip, 4);
        out[n].port = port;
        n++;
    };
    static const uint8_t kAll[4] = { 255, 255, 255, 255 };
    static const uint8_t kLoopback[4] = { 127, 0, 0, 1 };
    add(kAll);
    // A host on this machine is reached without the interface's broadcast.
    add(kLoopback);

#if defined(ONLINE_UWP)
    // An Xbox never hosts, so it has no LAN room to advertise and needs no interface list.
#elif defined(_WIN32)
    const std::vector<INTERFACE_INFO> info = interface_list();
    for (size_t i = 0; i < info.size(); i++)
    {
        if (!(info[i].iiFlags & IFF_UP) || (info[i].iiFlags & IFF_LOOPBACK) ||
            !(info[i].iiFlags & IFF_BROADCAST))
            continue;
        const uint32_t addr = info[i].iiAddress.AddressIn.sin_addr.s_addr;
        const uint32_t mask = info[i].iiNetmask.AddressIn.sin_addr.s_addr;
        const uint32_t bcast = addr | ~mask;
        add((const uint8_t*)&bcast);
    }
#elif !defined(__SWITCH__)
    ifaddrs* list = NULL;
    if (getifaddrs(&list) == 0)
    {
        for (ifaddrs* ifa = list; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
                !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) ||
                !(ifa->ifa_flags & IFF_BROADCAST) || ifa->ifa_netmask == NULL)
                continue;
            const uint32_t addr = ((const sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
            const uint32_t mask = ((const sockaddr_in*)ifa->ifa_netmask)->sin_addr.s_addr;
            const uint32_t bcast = addr | ~mask;
            add((const uint8_t*)&bcast);
        }
        freeifaddrs(list);
    }
#endif
    return n;
}

int NetLanAddresses(char out[][16], int max)
{
    struct Found
    {
        uint8_t ip[4];
        int rank;
    };
    std::vector<Found> found;
    // `onRoute`: the default route's address on a broadcast interface rather than a VPN's
    // point-to-point link. Replies leave from it, so it beats every prefix.
    auto consider = [&](const uint8_t ip[4], bool onRoute) {
        const int rank = lan_rank(ip) > 0 && onRoute ? 5 : lan_rank(ip);
        if (rank == 0)
            return;
        for (const Found& f : found)
            if (std::memcmp(f.ip, ip, 4) == 0)
                return;
        Found f;
        std::memcpy(f.ip, ip, 4);
        f.rank = rank;
        found.push_back(f);
    };

#if defined(ONLINE_UWP)
#elif defined(_WIN32)
    uint8_t routed[4];
    const bool haveRouted = routed_ipv4(routed);
    const std::vector<INTERFACE_INFO> info = interface_list();
    for (size_t i = 0; i < info.size(); i++)
    {
        const u_long flags = info[i].iiFlags;
        if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK))
            continue;
        const uint8_t* ip = (const uint8_t*)&info[i].iiAddress.AddressIn.sin_addr;
        consider(ip, haveRouted && std::memcmp(ip, routed, 4) == 0 && (flags & IFF_BROADCAST) &&
                         !(flags & IFF_POINTTOPOINT));
    }
#elif defined(__SWITCH__)
    // With no network, gethostid answers INADDR_LOOPBACK in host byte order.
    const uint32_t id = (uint32_t)gethostid();
    if (id != INADDR_LOOPBACK)
        consider((const uint8_t*)&id, false);
#else
    uint8_t routed[4];
    const bool haveRouted = routed_ipv4(routed);
    ifaddrs* list = NULL;
    if (getifaddrs(&list) == 0)
    {
        for (ifaddrs* ifa = list; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
                !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
                continue;
            const uint8_t* ip = (const uint8_t*)&((const sockaddr_in*)ifa->ifa_addr)->sin_addr;
            consider(ip, haveRouted && std::memcmp(ip, routed, 4) == 0 &&
                             (ifa->ifa_flags & IFF_BROADCAST) &&
                             !(ifa->ifa_flags & IFF_POINTOPOINT));
        }
        freeifaddrs(list);
    }
#endif
    std::stable_sort(found.begin(), found.end(),
                     [](const Found& x, const Found& y) { return x.rank > y.rank; });
    int n = 0;
    for (; n < max && n < (int)found.size(); n++)
        std::snprintf(out[n], 16, "%u.%u.%u.%u", found[n].ip[0], found[n].ip[1], found[n].ip[2],
                      found[n].ip[3]);
    return n;
}

bool NetLanAddress(char* out, size_t size)
{
    char first[1][16];
    if (NetLanAddresses(first, 1) == 0)
        return false;
    std::snprintf(out, size, "%s", first[0]);
    return true;
}

bool NetRandom(void* buf, size_t size)
{
#if defined(_WIN32)
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__SWITCH__)
    randomGet(buf, size);
    return true;
#else
    uint8_t* p = (uint8_t*)buf;
    while (size > 0)
    {
        const size_t n = size > 256 ? 256 : size;
        if (getentropy(p, n) != 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
#endif
}
}   // namespace online
