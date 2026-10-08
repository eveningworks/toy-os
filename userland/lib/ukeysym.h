#ifndef ULIB_UKEYSYM_H
#define ULIB_UKEYSYM_H

#include <stdint.h>

// An X11 KEYSYM -- what a VNC viewer sends for a key (RFB 7.5.4) -- as
// something toy-os's input core takes: a Latin-1 character to type on
// the active layout, or an evdev keycode (abi/input_keys.h) for a key
// that is not a character (arrows, F-keys, modifiers, the keypad).
//
// THE SPLIT IS THE KEYSYM'S OWN: 0x20-0x7E and 0xA0-0xFF are Latin-1
// by definition (X11's keysymdef.h), and the 0xFFxx block is function
// keys. A character goes through the layout so it lands on whichever
// key types it here, as x11vnc does; a function key has one keycode on
// every layout.

enum ukeysym_kind { UKEYSYM_NONE, UKEYSYM_CHAR, UKEYSYM_KEY };

// Classifies `keysym`; fills `*out` with the character or the keycode.
enum ukeysym_kind ukeysym_lookup(uint32_t keysym, uint16_t *out);

#endif
