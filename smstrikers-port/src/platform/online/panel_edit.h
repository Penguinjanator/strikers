// The online panel's text entry without a keyboard: a row of character cells a pad edits.

#ifndef PORT_ONLINE_PANEL_EDIT_H
#define PORT_ONLINE_PANEL_EDIT_H

#include "port/online.h"

#include <stddef.h>

namespace online_panel
{

struct Wheel
{
    char text[ONLINE_ADDRESS_MAX];
    int cursor;   // the cell being edited; text's length is the blank cell after it
};

enum WheelMove
{
    WHEEL_LEFT = 1 << 0,
    WHEEL_RIGHT = 1 << 1,
    WHEEL_UP = 1 << 2,     // the next character in the set
    WHEEL_DOWN = 1 << 3,   // the previous one
    WHEEL_DELETE = 1 << 4
};

extern const char kAddressChars[];
extern const char kNameChars[];
extern const char kDigitChars[];

// Applies WheelMove bits; on the text's last cell up and down also pass blank, which deletes it.
void wheel_edit(Wheel& w, const char* charset, int maxLen, unsigned moves);

// Sets the text, cut to `maxLen`, with the cursor after it, or on its last cell when full.
void wheel_set(Wheel& w, const char* text, int maxLen);

// `in` without control characters or surrounding blanks, and without spaces unless allowed.
void clean_field(char* out, size_t cap, const char* in, bool allowSpace);

// Printable ASCII with no spaces: what an address typed, pasted or read back may hold.
bool plain_address(const char* s);

}   // namespace online_panel

#endif   // PORT_ONLINE_PANEL_EDIT_H
