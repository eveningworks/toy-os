#ifndef USND_INTERNAL_H
#define USND_INTERNAL_H

#include "lib/usnd.h"

// Shared between usnd.c and its codecs. Not for apps.

// Records the sentence usnd_last_error() returns. A codec calls this on
// every refusal, because the errno alone cannot tell "not a WAV" from
// "a WAV this build will not play".
void usnd_fail(const char *msg);

// Decode a whole (already opened) stream into a clip, in the device
// format. Shared so usnd_clip_load() and usnd_clip_from_pcm() cannot
// drift: they differ only in where the samples come from.
int usnd_clip_drain(struct usnd_stream *s, struct usnd_clip *c);

#endif
