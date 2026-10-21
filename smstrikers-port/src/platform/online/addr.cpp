// Address text and classification, without the socket API.

#include "pipe.h"

#include "port/online.h"

#include <cstdio>
#include <cstring>

namespace online
{
namespace
{
bool parse_port(const char* s, uint16_t* out)
{
    if (*s == '\0')
        return false;
    unsigned long v = 0;
    for (; *s != '\0'; s++)
    {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > 65535)
            return false;
    }
    if (v == 0)
        return false;
    *out = (uint16_t)v;
    return true;
}

bool parse_ipv4(const char* s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++)
    {
        if (*s < '0' || *s > '9')
            return false;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || v > 255)
                return false;
            s++;
        }
        out[i] = (uint8_t)v;
        if (i < 3)
        {
            if (*s != '.')
                return false;
            s++;
        }
    }
    return *s == '\0';
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool parse_ipv6(const char* s, uint8_t out[16])
{
    uint16_t head[8];
    uint16_t tail[8];
    int nHead = 0;
    int nTail = 0;
    bool gap = false;

    if (s[0] == ':')
    {
        if (s[1] != ':')
            return false;
        gap = true;
        s += 2;
    }
    while (*s != '\0')
    {
        uint16_t* groups = gap ? tail : head;
        int* n = gap ? &nTail : &nHead;
        if (nHead + nTail >= 8)
            return false;

        // An embedded IPv4 address ends the text.
        const char* dot = std::strchr(s, '.');
        const char* colon = std::strchr(s, ':');
        if (dot != NULL && (colon == NULL || dot < colon))
        {
            uint8_t v4[4];
            if (nHead + nTail > 6 || !parse_ipv4(s, v4))
                return false;
            groups[(*n)++] = (uint16_t)(v4[0] << 8 | v4[1]);
            groups[(*n)++] = (uint16_t)(v4[2] << 8 | v4[3]);
            s += std::strlen(s);
            break;
        }

        unsigned v = 0;
        int digits = 0;
        while (hex_digit(*s) >= 0)
        {
            v = v << 4 | (unsigned)hex_digit(*s);
            if (++digits > 4)
                return false;
            s++;
        }
        if (digits == 0)
            return false;
        groups[(*n)++] = (uint16_t)v;
        if (*s == ':')
        {
            s++;
            if (*s == ':')
            {
                if (gap)
                    return false;
                gap = true;
                s++;
            }
            else if (*s == '\0')
                return false;
        }
        else if (*s != '\0')
            return false;
    }
    if (gap ? nHead + nTail > 7 : nHead != 8)
        return false;

    uint16_t all[8] = {};
    for (int i = 0; i < nHead; i++)
        all[i] = head[i];
    for (int i = 0; i < nTail; i++)
        all[8 - nTail + i] = tail[i];
    for (int i = 0; i < 8; i++)
    {
        out[2 * i] = (uint8_t)(all[i] >> 8);
        out[2 * i + 1] = (uint8_t)all[i];
    }
    return true;
}
}   // namespace

bool AddrEqual(const NetAddr& a, const NetAddr& b)
{
    if (a.family != b.family || a.port != b.port)
        return false;
    return std::memcmp(a.ip, b.ip, a.family == 4 ? 4 : 16) == 0;
}

void AddrFormat(const NetAddr& a, char* out, size_t size)
{
    if (a.family == 4)
    {
        std::snprintf(out, size, "%u.%u.%u.%u:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3],
                      (unsigned)a.port);
        return;
    }
    if (a.family != 6)
    {
        std::snprintf(out, size, "?");
        return;
    }

    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (uint16_t)(a.ip[2 * i] << 8 | a.ip[2 * i + 1]);
    // The longest run of two or more zero groups becomes "::".
    int best = -1;
    int bestLen = 1;
    for (int i = 0; i < 8;)
    {
        int j = i;
        while (j < 8 && g[j] == 0)
            j++;
        if (j - i > bestLen)
        {
            best = i;
            bestLen = j - i;
        }
        i = j > i ? j : i + 1;
    }

    char text[48];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
    {
        if (i == best)
        {
            n += (size_t)std::snprintf(text + n, sizeof text - n, "::");
            i += bestLen - 1;
            continue;
        }
        if (n > 0 && text[n - 1] != ':')
            text[n++] = ':';
        n += (size_t)std::snprintf(text + n, sizeof text - n, "%x", g[i]);
    }
    text[n] = '\0';
    std::snprintf(out, size, "[%s]:%u", text, (unsigned)a.port);
}

bool AddrIsLanSource(const NetAddr& a)
{
    if (a.family == 4)
    {
        const uint8_t* ip = a.ip;
        // 100.64/10 is carrier-grade NAT's range, and Tailscale's addresses.
        return ip[0] == 127 || ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) ||
               (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) ||
               (ip[0] == 100 && (ip[1] & 0xC0) == 64);
    }
    if (a.family == 6)
    {
        static const uint8_t kLoopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        return std::memcmp(a.ip, kLoopback, 16) == 0 ||
               (a.ip[0] == 0xFE && (a.ip[1] & 0xC0) == 0x80) || (a.ip[0] & 0xFE) == 0xFC;
    }
    return false;
}

bool AddrSplit(const char* text, char* host, size_t hostSize, uint16_t* port, uint16_t defaultPort)
{
    if (text == NULL)
        return false;
    while (*text == ' ' || *text == '\t')
        text++;
    char buf[ONLINE_ADDRESS_MAX];
    size_t n = std::strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' ||
                     text[n - 1] == '\n'))
        n--;
    if (n == 0 || n >= sizeof buf)
        return false;
    std::memcpy(buf, text, n);
    buf[n] = '\0';
    for (size_t i = 0; i < n; i++)
    {
        const unsigned char c = (unsigned char)buf[i];
        if (c <= ' ' || c == 0x7F || c == '/' || c == '\\' || c == '@')
            return false;
    }

    const char* h = buf;
    const char* p = NULL;
    if (buf[0] == '[')
    {
        char* close = std::strchr(buf, ']');
        if (close == NULL)
            return false;
        *close = '\0';
        h = buf + 1;
        if (close[1] == ':')
            p = close + 2;
        else if (close[1] != '\0')
            return false;
    }
    else
    {
        char* first = std::strchr(buf, ':');
        if (first != NULL && std::strchr(first + 1, ':') == NULL)
        {
            *first = '\0';
            p = first + 1;
        }
    }

    uint16_t v = defaultPort;
    if (p != NULL && !parse_port(p, &v))
        return false;
    const size_t hl = std::strlen(h);
    if (hl == 0 || hl >= hostSize)
        return false;
    std::memcpy(host, h, hl + 1);
    *port = v;
    return true;
}

bool AddrParseNumeric(const char* host, uint16_t port, NetAddr* out)
{
    NetAddr a;
    std::memset(&a, 0, sizeof a);
    a.port = port;
    if (parse_ipv4(host, a.ip))
        a.family = 4;
    else if (parse_ipv6(host, a.ip))
    {
        a.family = 6;
        static const uint8_t kMapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (std::memcmp(a.ip, kMapped, 12) == 0)
        {
            std::memmove(a.ip, a.ip + 12, 4);
            std::memset(a.ip + 4, 0, 12);
            a.family = 4;
        }
    }
    else
        return false;
    *out = a;
    return true;
}
}   // namespace online
