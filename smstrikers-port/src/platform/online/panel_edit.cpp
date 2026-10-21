#include "panel_edit.h"

#include <cstring>

namespace online_panel
{

// Digits first: most addresses typed on a pad are IPv4.
const char kAddressChars[] = "0123456789.:abcdefghijklmnopqrstuvwxyz-[]";
const char kNameChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.!?";
const char kDigitChars[] = "0123456789";

namespace
{

int index_of(const char* charset, char c)
{
    const char* at = c != '\0' ? std::strchr(charset, c) : nullptr;
    return at != nullptr ? (int)(at - charset) : -1;
}

int clamp_max(int maxLen)
{
    const int cap = (int)sizeof(Wheel::text) - 1;
    return maxLen < 1 ? 1 : (maxLen > cap ? cap : maxLen);
}

}   // namespace

void wheel_edit(Wheel& w, const char* charset, int maxLen, unsigned moves)
{
    maxLen = clamp_max(maxLen);
    w.text[sizeof w.text - 1] = '\0';
    int len = (int)std::strlen(w.text);
    if (len > maxLen)
    {
        w.text[maxLen] = '\0';
        len = maxLen;
    }
    const int last = len < maxLen ? len : maxLen - 1;
    if (w.cursor < 0)
        w.cursor = 0;
    if (w.cursor > last)
        w.cursor = last;

    if ((moves & WHEEL_LEFT) != 0 && w.cursor > 0)
        w.cursor--;
    if ((moves & WHEEL_RIGHT) != 0 && w.cursor < last)
        w.cursor++;

    const int dir = (moves & WHEEL_UP) != 0 ? 1 : (moves & WHEEL_DOWN) != 0 ? -1 : 0;
    const int n = (int)std::strlen(charset);
    if (dir != 0 && n > 0)
    {
        // Index n is the blank, offered only where removing a character leaves no gap.
        const int cycle = w.cursor >= len - 1 ? n + 1 : n;
        int i = w.cursor < len ? index_of(charset, w.text[w.cursor]) : n;
        if (i < 0)
            i = dir > 0 ? 0 : n - 1;
        else
            i = (i + dir + cycle) % cycle;
        if (i == n)
        {
            if (w.cursor < len)
                w.text[w.cursor] = '\0';
        }
        else if (w.cursor < len)
            w.text[w.cursor] = charset[i];
        else if (len < maxLen)
        {
            w.text[len] = charset[i];
            w.text[len + 1] = '\0';
        }
    }

    len = (int)std::strlen(w.text);
    if ((moves & WHEEL_DELETE) != 0 && len > 0)
    {
        const int at = w.cursor < len ? w.cursor : len - 1;
        std::memmove(w.text + at, w.text + at + 1, (size_t)(len - at));
        if (w.cursor > at)
            w.cursor = at;
    }
}

void wheel_set(Wheel& w, const char* text, int maxLen)
{
    maxLen = clamp_max(maxLen);
    size_t n = text != nullptr ? std::strlen(text) : 0;
    if (n > (size_t)maxLen)
        n = (size_t)maxLen;
    if (n > 0)
        std::memmove(w.text, text, n);
    w.text[n] = '\0';
    w.cursor = (int)n < maxLen ? (int)n : maxLen - 1;
}

void clean_field(char* out, size_t cap, const char* in, bool allowSpace)
{
    if (cap == 0)
        return;
    size_t n = 0;
    while (*in == ' ' || *in == '\t')
        in++;
    for (; *in != '\0' && n + 1 < cap; in++)
    {
        const unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7F || (!allowSpace && c == ' '))
            continue;
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

bool plain_address(const char* s)
{
    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++)
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 0x7F)
            return false;
    return true;
}

}   // namespace online_panel
