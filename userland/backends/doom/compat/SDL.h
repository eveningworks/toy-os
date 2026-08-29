#ifndef TOYOS_DOOM_COMPAT_SDL_H
#define TOYOS_DOOM_COMPAT_SDL_H

// A COMPATIBILITY SHIM, not a stub -- everything here really works.
//
// Chocolate Doom's OPL library and MIDI reader use exactly two things
// from SDL: big-endian byte swaps, and a mutex/condition pair inside
// `OPL_Delay()`. Both exist here already, so this maps them across
// rather than faking them, and `OPL_Delay()` genuinely blocks.
//
// It lives on OUR side of the boundary, not in userland/ports/doom/, so
// the vendored files stay byte for byte upstream (see that directory's
// README). Only files compiled with -Iuserland/backends/doom/compat can see it.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>   // opl.c reaches strcmp through SDL.h upstream
#include <pthread.h>

// Doom's MIDI headers are big-endian on a little-endian machine.
// __builtin_bswap is the compiler's, so this is one instruction.
#define SDL_SwapBE16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define SDL_SwapBE32(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))

typedef pthread_mutex_t SDL_mutex;
typedef pthread_cond_t  SDL_cond;

static inline SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = malloc(sizeof *m);
    if (m) pthread_mutex_init(m, 0);
    return m;
}
static inline void SDL_DestroyMutex(SDL_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(m);
    free(m);
}
static inline int SDL_LockMutex(SDL_mutex *m)   { return m ? pthread_mutex_lock(m) : -1; }
static inline int SDL_UnlockMutex(SDL_mutex *m) { return m ? pthread_mutex_unlock(m) : -1; }

static inline SDL_cond *SDL_CreateCond(void) {
    SDL_cond *c = malloc(sizeof *c);
    if (c) pthread_cond_init(c, 0);
    return c;
}
static inline void SDL_DestroyCond(SDL_cond *c) {
    if (!c) return;
    pthread_cond_destroy(c);
    free(c);
}
static inline int SDL_CondSignal(SDL_cond *c) { return c ? pthread_cond_signal(c) : -1; }
static inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m) {
    return (c && m) ? pthread_cond_wait(c, m) : -1;
}

#endif
