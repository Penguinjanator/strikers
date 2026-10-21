// The online panel's character wheel and field rules, and its catalogue's printf arguments.

#include <stdio.h>
#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "panel_edit.h"
#include "panel_text.h"

using namespace online_panel;

static int cases;

static void check(bool result, const char* name)
{
    ++cases;
    if (!result)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        exit(1);
    }
}

static Wheel wheel(const char* text, int cursor)
{
    Wheel w;
    memset(&w, 0, sizeof w);
    snprintf(w.text, sizeof w.text, "%s", text);
    w.cursor = cursor;
    return w;
}

static bool is(const Wheel& w, const char* text, int cursor)
{
    return strcmp(w.text, text) == 0 && w.cursor == cursor;
}

// The conversions in a printf format, one letter each, "%%" skipped; '?' for anything else.
static void conversions(const char* f, char* out, size_t cap)
{
    size_t n = 0;
    for (; *f != '\0'; f++)
    {
        if (*f != '%')
            continue;
        f++;
        if (*f == '%')
            continue;
        while (*f == '+' || *f == '-' || *f == ' ' || *f == '.' || (*f >= '0' && *f <= '9'))
            f++;
        const char c = (*f == 's' || *f == 'd' || *f == 'u' || *f == 'f') ? *f : '?';
        if (n + 1 < cap)
            out[n++] = c;
        if (*f == '\0')
            break;
    }
    out[n] = '\0';
}

int main()
{
    const char* digits = kDigitChars;

    // An empty wheel: up takes the set's first character, down its last.
    {
        Wheel w = wheel("", 0);
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "0", 0), "up on empty appends the first character");
        w = wheel("", 0);
        wheel_edit(w, digits, 5, WHEEL_DOWN);
        check(is(w, "9", 0), "down on empty appends the last character");
    }

    // Right stops on the blank cell after the text and never past it.
    {
        Wheel w = wheel("12", 0);
        wheel_edit(w, digits, 5, WHEEL_RIGHT);
        wheel_edit(w, digits, 5, WHEEL_RIGHT);
        check(is(w, "12", 2), "right reaches the blank cell");
        wheel_edit(w, digits, 5, WHEEL_RIGHT);
        check(is(w, "12", 2), "right stops at the blank cell");
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "120", 2), "up on the blank cell appends");
        wheel_edit(w, digits, 5, WHEEL_LEFT | WHEEL_LEFT);
        check(w.cursor == 1, "left moves one cell a frame");
        w.cursor = 0;
        wheel_edit(w, digits, 5, WHEEL_LEFT);
        check(w.cursor == 0, "left stops at the first cell");
    }

    // A full wheel keeps its cursor on the last character.
    {
        Wheel w = wheel("41210", 4);
        wheel_edit(w, digits, 5, WHEEL_RIGHT);
        check(is(w, "41210", 4), "no blank cell past a full wheel");
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "41211", 4), "up changes the last character of a full wheel");
        w = wheel("41210", 9);
        wheel_edit(w, digits, 5, 0);
        check(w.cursor == 4, "an out-of-range cursor is clamped");
    }

    // Up and down wrap in the middle of the text and pass through blank only at its end.
    {
        Wheel w = wheel("19", 0);
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "29", 0), "up in the middle");
        w = wheel("09", 0);
        wheel_edit(w, digits, 5, WHEEL_DOWN);
        check(is(w, "99", 0), "down wraps in the middle without a blank");
        w = wheel("19", 1);
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "1", 1), "up past the set's end on the last character removes it");
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "10", 1), "then up again brings the first character back");
        w = wheel("10", 1);
        wheel_edit(w, digits, 5, WHEEL_DOWN);
        check(is(w, "1", 1), "down from the first character on the last cell removes it");
    }

    // A character outside the set steps to the set's first or last.
    {
        Wheel w = wheel("a", 0);
        wheel_edit(w, digits, 5, WHEEL_UP);
        check(is(w, "0", 0), "unknown character, up");
        w = wheel("a1", 0);
        wheel_edit(w, digits, 5, WHEEL_DOWN);
        check(is(w, "91", 0), "unknown character, down");
    }

    // Delete removes the character under the cursor, or the last one from the blank cell.
    {
        Wheel w = wheel("123", 1);
        wheel_edit(w, digits, 5, WHEEL_DELETE);
        check(is(w, "13", 1), "delete in the middle");
        w = wheel("123", 2);
        wheel_edit(w, digits, 5, WHEEL_DELETE);
        check(is(w, "12", 2), "delete the last character");
        w = wheel("123", 3);
        wheel_edit(w, digits, 5, WHEEL_DELETE);
        check(is(w, "12", 2), "delete from the blank cell");
        w = wheel("", 0);
        wheel_edit(w, digits, 5, WHEEL_DELETE);
        check(is(w, "", 0), "delete on empty");
    }

    // Text longer than the limit is cut, and a set with no characters changes nothing.
    {
        Wheel w = wheel("1234567", 6);
        wheel_edit(w, digits, 5, 0);
        check(is(w, "12345", 4), "cut to the limit");
        w = wheel("12", 1);
        wheel_edit(w, "", 5, WHEEL_UP);
        check(is(w, "12", 1), "empty set");
        w = wheel("", 0);
        wheel_edit(w, digits, 1000, WHEEL_UP);
        check(is(w, "0", 0), "a limit past the buffer is clamped");
    }

    // Every address character a pad can reach is one an address may hold.
    {
        Wheel w = wheel("", 0);
        const int n = (int)strlen(kAddressChars);
        for (int i = 0; i < n; i++)
        {
            w.text[0] = kAddressChars[i];
            w.text[1] = '\0';
            check(plain_address(w.text), "address character is plain");
        }
        w = wheel("192.168.1.20:41210", 18);
        wheel_edit(w, kAddressChars, ONLINE_ADDRESS_MAX - 1, WHEEL_LEFT | WHEEL_UP);
        check(strcmp(w.text, "192.168.1.20:41211") == 0, "address wheel");
    }

    {
        Wheel w = wheel("x", 0);
        wheel_set(w, "abc", 16);
        check(is(w, "abc", 3), "set leaves the cursor on the blank cell");
        wheel_set(w, "abcdef", 4);
        check(is(w, "abcd", 3), "set cuts to the limit");
        wheel_set(w, w.text, 16);
        check(is(w, "abcd", 4), "set from its own text");
        wheel_set(w, nullptr, 16);
        check(is(w, "", 0), "set to nothing");
    }

    {
        char out[32];
        clean_field(out, sizeof out, "  12.0.0.1 \t", false);
        check(strcmp(out, "12.0.0.1") == 0, "address trimmed");
        clean_field(out, sizeof out,
                    "a b\x01\x7F"
                    "c",
                    false);
        check(strcmp(out, "abc") == 0, "spaces and controls dropped");
        clean_field(out, sizeof out, "  Sam  Two  ", true);
        check(strcmp(out, "Sam  Two") == 0, "inner spaces kept in a name");
        clean_field(out, 4, "abcdef", false);
        check(strcmp(out, "abc") == 0, "cut to the buffer");
        check(plain_address("host:41210"), "plain");
        check(!plain_address(""), "empty is not an address");
        check(!plain_address("a b"), "a space is not plain");
        check(!plain_address("\xC3\xA9"), "UTF-8 is not plain");
    }

    // Every string exists and takes the arguments its entry says, so a translation can be checked
    // the same way.
    for (const char* language : {"en", "de", "fr", "es"})
    {
    text_language(language);
    for (int i = 0; i < T_COUNT; i++)
    {
        const char* s = text((Text)i);
        char name[48];
        snprintf(name, sizeof name, "text %d present", i);
        check(s[0] != '\0', name);
        char got[16];
        conversions(s, got, sizeof got);
        snprintf(name, sizeof name, "text %d arguments", i);
        check(strcmp(got, text_args((Text)i)) == 0, name);
    }
    }
    check(text(T_COUNT)[0] == '\0', "out-of-range text is empty");

    text_language("fr");
    check(strcmp(text(T_CANCEL), "Annuler") == 0, "French settings terminology");
    unsigned short native[128];
    text_utf16(T_CHANGE_TEAM, native, 128, true);
    check(native[0] == 'C' && native[10] == 0xC9, "native button label preserves translated accents");
    char upper[128];
    text_upper("équipe Straße œuf", upper, sizeof upper);
    check(strcmp(upper, "ÉQUIPE STRASSE ŒUF") == 0, "accented headings remain valid uppercase UTF-8");
    char small[2] = {'x', 'x'};
    text_upper("é", small, sizeof small);
    check(small[0] == 0, "truncation preserves UTF-8 character boundaries");
    const unsigned short stadium[] = {'S', 0xE9, 0xD83C, 0xDFDF, 0};
    text_native(T_STADIUM_FIRST, stadium);
    text_utf16(T_STADIUM_FIRST, native, 128);
    check(memcmp(stadium, native, sizeof stadium) == 0, "native names round trip including surrogate pairs");
    text_utf16(T_STADIUM_FIRST, native, 4);
    check(native[2] == 0, "UTF-16 truncation preserves surrogate pairs");
    text_language("unsupported");
    check(strcmp(text(T_CANCEL), "Cancel") == 0, "unsupported language uses English");
    check(strcmp(text(T_STADIUM_FIRST), "Pipeline Central") == 0, "language change clears native names");

    printf("online_panel: %d cases\n", cases);
    return 0;
}
