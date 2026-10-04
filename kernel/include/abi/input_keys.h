#ifndef INPUT_KEYS_H
#define INPUT_KEYS_H

// KEY POSITIONS, IN LINUX'S evdev NUMBERING (input-event-codes.h) -- what
// the wire carries before any layout: INPUT_KEY_Q is the key left of W
// on every keyboard, whatever it prints. The kernel's drivers report
// these (kernel/input.h), and a window that asks for physical keys
// receives them (win_proto.h's WIN_EV_KEY_PHYS).
//
// THE `INPUT_` PREFIX IS NOT DECORATION. api/keyboard.h's `KEY_*` family
// means something else entirely -- the TRANSLATED codes a key ring
// carries (KEY_HOME is 0xF797, INPUT_KEY_HOME is 102). Four collided when
// the kernel's list was first written, and a silent collision between
// two key vocabularies surfaces as "Home does something odd on one
// keyboard".
//
// The standard 105-key block; a media key has a number past it and no
// name here.
#define INPUT_KEY_ESC 1
#define INPUT_KEY_1 2
#define INPUT_KEY_2 3
#define INPUT_KEY_3 4
#define INPUT_KEY_4 5
#define INPUT_KEY_5 6
#define INPUT_KEY_6 7
#define INPUT_KEY_7 8
#define INPUT_KEY_8 9
#define INPUT_KEY_9 10
#define INPUT_KEY_0 11
#define INPUT_KEY_MINUS 12
#define INPUT_KEY_EQUAL 13
#define INPUT_KEY_BACKSPACE 14
#define INPUT_KEY_TAB 15
#define INPUT_KEY_Q 16
#define INPUT_KEY_W 17
#define INPUT_KEY_E 18
#define INPUT_KEY_R 19
#define INPUT_KEY_T 20
#define INPUT_KEY_Y 21
#define INPUT_KEY_U 22
#define INPUT_KEY_I 23
#define INPUT_KEY_O 24
#define INPUT_KEY_P 25
#define INPUT_KEY_LEFTBRACE 26
#define INPUT_KEY_RIGHTBRACE 27
#define INPUT_KEY_ENTER 28
#define INPUT_KEY_LEFTCTRL 29
#define INPUT_KEY_A 30
#define INPUT_KEY_S 31
#define INPUT_KEY_D 32
#define INPUT_KEY_F 33
#define INPUT_KEY_G 34
#define INPUT_KEY_H 35
#define INPUT_KEY_J 36
#define INPUT_KEY_K 37
#define INPUT_KEY_L 38
#define INPUT_KEY_SEMICOLON 39
#define INPUT_KEY_APOSTROPHE 40
#define INPUT_KEY_GRAVE 41
#define INPUT_KEY_LEFTSHIFT 42
#define INPUT_KEY_BACKSLASH 43
#define INPUT_KEY_Z 44
#define INPUT_KEY_X 45
#define INPUT_KEY_C 46
#define INPUT_KEY_V 47
#define INPUT_KEY_B 48
#define INPUT_KEY_N 49
#define INPUT_KEY_M 50
#define INPUT_KEY_COMMA 51
#define INPUT_KEY_DOT 52
#define INPUT_KEY_SLASH 53
#define INPUT_KEY_RIGHTSHIFT 54
#define INPUT_KEY_KPASTERISK 55
#define INPUT_KEY_LEFTALT 56
#define INPUT_KEY_SPACE 57
#define INPUT_KEY_CAPSLOCK 58
#define INPUT_KEY_F1 59
#define INPUT_KEY_F2 60
#define INPUT_KEY_F3 61
#define INPUT_KEY_F4 62
#define INPUT_KEY_F5 63
#define INPUT_KEY_F6 64
#define INPUT_KEY_F7 65
#define INPUT_KEY_F8 66
#define INPUT_KEY_F9 67
#define INPUT_KEY_F10 68
#define INPUT_KEY_NUMLOCK 69
#define INPUT_KEY_SCROLLLOCK 70
#define INPUT_KEY_KP7 71
#define INPUT_KEY_KP8 72
#define INPUT_KEY_KP9 73
#define INPUT_KEY_KPMINUS 74
#define INPUT_KEY_KP4 75
#define INPUT_KEY_KP5 76
#define INPUT_KEY_KP6 77
#define INPUT_KEY_KPPLUS 78
#define INPUT_KEY_KP1 79
#define INPUT_KEY_KP2 80
#define INPUT_KEY_KP3 81
#define INPUT_KEY_KP0 82
#define INPUT_KEY_KPDOT 83
// The extra key an ISO keyboard has between Left Shift and Z; on every
// Nordic layout it carries `<`, `>` and, with AltGr, `|`.
#define INPUT_KEY_102ND 86
#define INPUT_KEY_F11 87
#define INPUT_KEY_F12 88
#define INPUT_KEY_KPENTER 96
#define INPUT_KEY_RIGHTCTRL 97
#define INPUT_KEY_KPSLASH 98
#define INPUT_KEY_SYSRQ 99      // Print Screen
#define INPUT_KEY_RIGHTALT 100  // AltGr on a layout that has one
#define INPUT_KEY_HOME 102
#define INPUT_KEY_UP 103
#define INPUT_KEY_PAGEUP 104
#define INPUT_KEY_LEFT 105
#define INPUT_KEY_RIGHT 106
#define INPUT_KEY_END 107
#define INPUT_KEY_DOWN 108
#define INPUT_KEY_PAGEDOWN 109
#define INPUT_KEY_INSERT 110
#define INPUT_KEY_DELETE 111
#define INPUT_KEY_PAUSE 119
#define INPUT_KEY_LEFTMETA 125
#define INPUT_KEY_RIGHTMETA 126
#define INPUT_KEY_COMPOSE 127

#endif
