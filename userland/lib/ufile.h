#ifndef ULIB_UFILE_H
#define ULIB_UFILE_H

#include <stddef.h>
#include <stdint.h>

// Reading a whole file into one allocation, in one place.
//
// WHY THIS IS SHARED. Three callers wrote the same twenty lines --
// /bin/install for boot.img and core.img, lib/uimg.c for a picture,
// ui/ugfx.c for a .ttf -- and the part that is easy to get wrong is
// invisible in all three: sys_read() MAY RETURN SHORT, so the read is a
// loop, and a single call that happened to fill the whole file on the
// shipped filesystem is a latent bug on any other one. Each of the
// three had the loop; a fourth would have been a coin toss.
//
// IT REPORTS AN OUTCOME, NOT AN ERRNO, and that is the whole reason it
// is not `int`. Its callers word their failures differently and for
// good reason -- uimg's reach a person in the Image Viewer, install's
// reach a transcript -- and mapping them through errno loses the
// distinction that matters most here: EMPTY and TOO_BIG are both
// EINVAL, and "the file is larger than this decoder will read" is not
// the same sentence as "the file is empty".
enum ufile_result {
    UFILE_OK,
    UFILE_NOENT,    // stat failed: no such file, or no permission
    UFILE_EMPTY,    // zero bytes; never a valid input for any caller here
    UFILE_TOO_BIG,  // larger than `cap`
    UFILE_NOMEM,    // the allocation failed
    UFILE_OPEN,     // stat succeeded and open did not -- a race, or a directory
    UFILE_SHORT,    // the read ended early; the file changed under us
};

// On UFILE_OK, `*out` is a fresh allocation of `*out_len` bytes that the
// CALLER FREES; on anything else `*out` is left NULL and nothing is
// allocated. `cap` of 0 means no ceiling.
//
// A file is REFUSED for being too large rather than read as a prefix:
// a truncated JPEG decodes, to a grey-tailed picture that reads as a
// decoder bug rather than as a file that did not fit.
enum ufile_result ufile_slurp(const char *path, size_t cap,
                              uint8_t **out, size_t *out_len);

// The first `cap` bytes of a file, for a caller deciding WHAT a file is
// rather than reading it: Image Viewer and the Audio Player both sniff
// a header before listing a name, so a WAV saved as .snd is listed and
// a text file called track.wav is not. Returns the byte count, or 0 --
// a short file is not an error here, it is an answer the probe uses.
//
// It does NOT loop. One read is the point: the caller wants a cheap
// look, and a header that arrives split across two reads is a file
// being written underneath it, which is not one to list either.
size_t ufile_read_head(const char *path, uint8_t *buf, size_t cap);

#endif // ULIB_UFILE_H
