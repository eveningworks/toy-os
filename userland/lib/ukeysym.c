// See ukeysym.h. The keysym numbers are X11's keysymdef.h.
#include "lib/ukeysym.h"
#include "input_keys.h"

static const struct { uint16_t sym; uint16_t key; } FN[] = {
    { 0xFF08, INPUT_KEY_BACKSPACE }, { 0xFF09, INPUT_KEY_TAB },
    { 0xFF0D, INPUT_KEY_ENTER },     { 0xFF13, INPUT_KEY_PAUSE },
    { 0xFF14, INPUT_KEY_SCROLLLOCK },{ 0xFF15, INPUT_KEY_SYSRQ },
    { 0xFF1B, INPUT_KEY_ESC },       { 0xFF50, INPUT_KEY_HOME },
    { 0xFF51, INPUT_KEY_LEFT },      { 0xFF52, INPUT_KEY_UP },
    { 0xFF53, INPUT_KEY_RIGHT },     { 0xFF54, INPUT_KEY_DOWN },
    { 0xFF55, INPUT_KEY_PAGEUP },    { 0xFF56, INPUT_KEY_PAGEDOWN },
    { 0xFF57, INPUT_KEY_END },       { 0xFF61, INPUT_KEY_SYSRQ },
    { 0xFF63, INPUT_KEY_INSERT },    { 0xFF67, INPUT_KEY_COMPOSE },
    { 0xFF7F, INPUT_KEY_NUMLOCK },   { 0xFFFF, INPUT_KEY_DELETE },
    // The keypad, Num Lock on and off alike: the key is the same key.
    { 0xFF8D, INPUT_KEY_KPENTER },   { 0xFFAA, INPUT_KEY_KPASTERISK },
    { 0xFFAB, INPUT_KEY_KPPLUS },    { 0xFFAD, INPUT_KEY_KPMINUS },
    { 0xFFAE, INPUT_KEY_KPDOT },     { 0xFF9F, INPUT_KEY_KPDOT },
    { 0xFFAF, INPUT_KEY_KPSLASH },
    { 0xFFB0, INPUT_KEY_KP0 }, { 0xFF9E, INPUT_KEY_KP0 },
    { 0xFFB1, INPUT_KEY_KP1 }, { 0xFF9C, INPUT_KEY_KP1 },
    { 0xFFB2, INPUT_KEY_KP2 }, { 0xFF99, INPUT_KEY_KP2 },
    { 0xFFB3, INPUT_KEY_KP3 }, { 0xFF9B, INPUT_KEY_KP3 },
    { 0xFFB4, INPUT_KEY_KP4 }, { 0xFF96, INPUT_KEY_KP4 },
    { 0xFFB5, INPUT_KEY_KP5 }, { 0xFF9D, INPUT_KEY_KP5 },
    { 0xFFB6, INPUT_KEY_KP6 }, { 0xFF98, INPUT_KEY_KP6 },
    { 0xFFB7, INPUT_KEY_KP7 }, { 0xFF95, INPUT_KEY_KP7 },
    { 0xFFB8, INPUT_KEY_KP8 }, { 0xFF97, INPUT_KEY_KP8 },
    { 0xFFB9, INPUT_KEY_KP9 }, { 0xFF9A, INPUT_KEY_KP9 },
    { 0xFFBE, INPUT_KEY_F1 },  { 0xFFBF, INPUT_KEY_F2 },
    { 0xFFC0, INPUT_KEY_F3 },  { 0xFFC1, INPUT_KEY_F4 },
    { 0xFFC2, INPUT_KEY_F5 },  { 0xFFC3, INPUT_KEY_F6 },
    { 0xFFC4, INPUT_KEY_F7 },  { 0xFFC5, INPUT_KEY_F8 },
    { 0xFFC6, INPUT_KEY_F9 },  { 0xFFC7, INPUT_KEY_F10 },
    { 0xFFC8, INPUT_KEY_F11 }, { 0xFFC9, INPUT_KEY_F12 },
    { 0xFFE1, INPUT_KEY_LEFTSHIFT }, { 0xFFE2, INPUT_KEY_RIGHTSHIFT },
    { 0xFFE3, INPUT_KEY_LEFTCTRL },  { 0xFFE4, INPUT_KEY_RIGHTCTRL },
    { 0xFFE5, INPUT_KEY_CAPSLOCK },
    { 0xFFE7, INPUT_KEY_LEFTMETA },  { 0xFFE8, INPUT_KEY_RIGHTMETA },
    { 0xFFE9, INPUT_KEY_LEFTALT },
    // Right Alt is AltGr here, as on every layout toy-os ships with one;
    // ISO_Level3_Shift is what a viewer on such a layout sends for it.
    { 0xFFEA, INPUT_KEY_RIGHTALT },  { 0xFE03, INPUT_KEY_RIGHTALT },
    { 0xFFEB, INPUT_KEY_LEFTMETA },  { 0xFFEC, INPUT_KEY_RIGHTMETA },
};

enum ukeysym_kind ukeysym_lookup(uint32_t keysym, uint16_t *out) {
    if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF)) {
        *out = (uint16_t)keysym;
        return UKEYSYM_CHAR;
    }
    for (unsigned i = 0; i < sizeof FN / sizeof FN[0]; i++) {
        if (FN[i].sym == keysym) {
            *out = FN[i].key;
            return UKEYSYM_KEY;
        }
    }
    return UKEYSYM_NONE;
}
