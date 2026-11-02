/* libFuzzer target for the render path. The server hands user text straight to the engine,
 * so anything reachable from here is reachable over HTTP. The XML branch covers the
 * engine's markup parser, which is the more interesting surface.
 *
 * Voice data comes from SAM_DATA_DIR, the same variable the server reads. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sam_tts.h"

#define MAX_TEXT 4096

static sam_speech *open_or_die(const char *dir)
{
    char err[256] = "";
    sam_speech *t = sam_tts_open(dir, "Sam", err, sizeof err);
    if (!t) {
        /* a run that never reached the engine would report success while testing nothing */
        fprintf(stderr, "fuzz: cannot open Sam in SAM_DATA_DIR=%s: %s\n", dir, err);
        abort();
    }
    return t;
}

static int discard(const int16_t *samples, size_t n, void *user)
{
    (void)samples;
    (void)n;
    (void)user;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2) return 0;

    /* one byte of the input selects the branch, the rest is the text */
    int xml = ((unsigned)data[0] & 1U) ? 1 : 0;
    size_t len = size - 1;
    if (len > MAX_TEXT) len = MAX_TEXT;

    static char text[MAX_TEXT + 1];
    memcpy(text, data + 1, len);
    text[len] = 0;
    /* embedded NULs truncate the C string and stop the engine early, which hides the rest
     * of the input from the parser rather than exercising it */
    if (memchr(text, 0, len)) return 0;

    /* open and close per input, the way the server does: a pooled handle changes the
     * allocation pattern, and an out-of-bounds read only shows up as a crash when the
     * buffer it runs off the end of is the last one */
    const char *dir = getenv("SAM_DATA_DIR");
    if (!dir) dir = "data/voice";
    sam_speech *t = open_or_die(dir);
    sam_callbacks cb = {discard, NULL, NULL};
    sam_tts_speak(t, text, xml ? SAM_SPEAK_XML : 0, &cb);
    sam_tts_close(t);
    return 0;
}
