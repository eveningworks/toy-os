#ifndef TOYOS_DOOM_COMPAT_SDL_MIXER_H
#define TOYOS_DOOM_COMPAT_SDL_MIXER_H

// A SHIM, and deliberately empty.
//
// `i_sound.c` includes <SDL_mixer.h> under FEATURE_SOUND and never uses
// a symbol from it -- the SDL port's actual mixer code lives in
// `i_sdlsound.c`, which doomgeneric does not ship. Enabling the flag we
// need for the module list therefore drags in a header for a library
// this system does not have and does not want.
//
// The alternatives were worse. Editing the vendored file breaks the
// byte-for-byte rule that directory's README exists to protect, and
// `-D__DJGPP__` (which also suppresses the include) is a lie about the
// compiler that changes real behaviour in five other files -- byte
// swapping in i_swap.h, the ENDOOM screen, and i_system.c's exit path.
//
// If a future doomgeneric drops the include, delete this file.

#endif
