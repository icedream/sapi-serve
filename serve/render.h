/* Rendering boundary for sam_serve: what a speech request carries in, what the
 * engine produced, and how a request was rejected. Nothing in here knows about
 * sockets or HTTP; sam_serve.c owns the transport and turns these into responses.
 * The HTTP status lives in the structs only so the reason for a rejection travels
 * with it. */
#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <cjson/cJSON.h>

typedef struct {
    char voice[64];
    char model[64];
    char input[16384];
    char input_type[32];      /* "text" or "sapi-xml" / "sapi5-xml" */
    char response_format[32]; /* "wav" or "json" */
    char effect[32];
    double speed;           /* clamped to the range the engine can reach */
    double pitch_semitones; /* clamped to [-5, 5] */
    int volume;             /* clamped to [0, 100] */
    int word_timestamps;
    int have_rate;        /* an explicit rate overrides speed */
    int raw_rate;         /* before the engine's [-10, 18] clamp */
    size_t max_pcm_bytes; /* duration cap, bytes of 16-bit mono PCM */
} render_req;

typedef struct {
    int status;    /* HTTP status worth sending for this failure */
    char code[40]; /* machine-readable error code */
    char message[320];
} render_error;

typedef struct {
    char text[120];
    double start;
    double end; /* the following word's start, or the end of the audio */
} render_word;

typedef struct {
    unsigned char *audio; /* malloc'd 16-bit mono PCM, no container */
    size_t audio_len;
    render_word *words; /* malloc'd, NULL when not requested */
    int nwords;
    int sample_rate;
    int rate;     /* the engine rate actually applied, [-10, 18] */
    double speed; /* what that rate means: 3^(rate/10) */
    double pitch_semitones;
    int volume;
    double seconds;
    char voice[64];     /* the canonical on-disk name that was rendered */
    int effect_ignored; /* the engine rejected the effect name */
} render_out;

/* Maps the JSON body onto render_req and applies the documented clamps. `sam` is the
 * vendor-extension object; keys there win over the top-level ones. Returns 0 and fills
 * err when the request is not usable. Voice names are not checked here: the caller owns
 * the voice table, and so the model-to-voice resolution that happens when no voice is
 * given. */
int render_parse(const cJSON *body, const cJSON *sam, render_req *req, render_error *err);

/* Opens a handle, speaks once, closes it. One handle per request is not a shortcut: the
 * engine's accent prominence advances process-global rand() state across speaks on a
 * single handle, so two renders on the same handle would not be independent.
 * Returns 0 on success; on failure err says why and out holds nothing usable.
 * out->audio and out->words are freed by render_out_free. */
int render_speech(const char *data_dir, const render_req *req, render_out *out, render_error *err);

/* Response bodies for the two response_format values. Both return malloc'd memory. */
unsigned char *render_wav(const render_out *out, size_t *len);
char *render_metadata_json(const render_out *out);

void render_out_free(render_out *out);

#endif /* RENDER_H */
