#ifndef USND_INTERNAL_H
#define USND_INTERNAL_H

// Shared between usnd.c and its codecs. Not for apps.

// Records the sentence usnd_last_error() returns. A codec calls this on
// every refusal, because the errno alone cannot tell "not a WAV" from
// "a WAV this build will not play".
void usnd_fail(const char *msg);

#endif
