/* Rendering for sam_serve: engine callbacks, request field mapping, one speak per
 * request, and the bytes of both response bodies. The HTTP layer stays in
 * sam_serve.c. */
#include "render.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sam_tts.h"

#define MAX_WORDS 2000

/* The engine can only reach these ranges: rate is a divisor of 3^(rate/10) on
 * duration, and pitch is stored in half-semitones. Requests outside the range are
 * clamped rather than rejected, matching how SAPI itself behaves. */
#define SPEED_MIN 0.33
#define SPEED_MAX 7.22
#define PITCH_MIN (-5.0)
#define PITCH_MAX 5.0
#define RATE_MIN (-10)
#define RATE_MAX 18
#define VOLUME_MIN 0
#define VOLUME_MAX 100

typedef struct {
    unsigned char *pcm;
    size_t len, cap;
    int overflow;
} pcm_buf;

typedef struct {
    double start;
    char w[120];
} word_t;

typedef struct {
    pcm_buf pcm;
    word_t *word; /* heap: musl gives a thread 128 KB of stack */
    int nwords;
    const char *text; /* the exact UTF-8 given to sam_tts_speak, for text_pos/len */
    int rate_hz;
} render_ctx;

static void fail(render_error *err, int status, const char *code, const char *message)
{
    err->status = status;
    snprintf(err->code, sizeof err->code, "%s", code);
    snprintf(err->message, sizeof err->message, "%s", message);
}

/* --------------------------------------------------------- engine callbacks */

static int pcm_cb(const int16_t *samples, size_t n, void *user)
{
    pcm_buf *b = user;
    size_t add = n * sizeof(int16_t);
    if (b->len + add > b->cap) {
        b->overflow = 1;
        return 1;
    } /* return value 1 stops the speak */
    if (!b->pcm) {
        b->pcm = malloc(add);
    } else {
        /* realloc into a temp: assigning straight back to b->pcm loses the original
         * buffer if realloc fails */
        unsigned char *grown = realloc(b->pcm, b->len + add);
        if (!grown) {
            b->overflow = 1;
            return 1;
        }
        b->pcm = grown;
    }
    if (!b->pcm) {
        b->overflow = 1;
        return 1;
    }
    memcpy(b->pcm + b->len, samples, add);
    b->len += add;
    return 0;
}

static void event_cb(const sam_event *ev, void *user)
{
    render_ctx *c = user;
    if (ev->type != SAM_EV_WORD) return;
    if (c->nwords >= MAX_WORDS || !c->text || ev->text_pos < 0 || ev->text_len <= 0) return;
    size_t len = (size_t)ev->text_len;
    if (len > sizeof c->word[0].w - 1) len = sizeof c->word[0].w - 1;
    memcpy(c->word[c->nwords].w, c->text + ev->text_pos, len);
    c->word[c->nwords].w[len] = 0;
    c->word[c->nwords].start = (double)ev->audio_pos / (double)c->rate_hz;
    c->nwords++;
}

/* ------------------------------------------------------------- WAV writing */

static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4U; i++)
        p[i] = (unsigned char)(v >> (8U * i));
}

/* 44-byte canonical PCM WAV header. SAPI's own header is a different length, so the
 * comparison tooling parses the container instead of assuming offsets. */
static void wav_header(unsigned char *h, uint32_t rate, uint32_t datalen)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + datalen);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    h[20] = 1;
    h[21] = 0; /* PCM */
    h[22] = 1;
    h[23] = 0; /* mono */
    put32(h + 24, rate);
    put32(h + 28, rate * 2); /* byte rate */
    h[32] = 2;
    h[33] = 0; /* block align */
    h[34] = 16;
    h[35] = 0; /* bits per sample */
    memcpy(h + 36, "data", 4);
    put32(h + 40, datalen);
}

/* ------------------------------------------------------------ request JSON */

/* Keys under "sam" are the vendor extensions and win over the top-level ones, so a
 * client can send the OpenAI shape and still reach the Sam-specific fields. */
static const cJSON *field(const cJSON *body, const cJSON *sam, const char *key)
{
    const cJSON *v = sam ? cJSON_GetObjectItemCaseSensitive(sam, key) : NULL;
    return v ? v : cJSON_GetObjectItemCaseSensitive(body, key);
}

static double num_field(const cJSON *body, const cJSON *sam, const char *key, double fallback)
{
    const cJSON *v = field(body, sam, key);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : fallback;
}

static int bool_field(const cJSON *body, const cJSON *sam, const char *key, int fallback)
{
    const cJSON *v = field(body, sam, key);
    if (!v) return fallback;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return fallback;
}

static void str_field(const cJSON *body, const cJSON *sam, const char *key, char *out, size_t outsz)
{
    const cJSON *v = field(body, sam, key);
    if (v && cJSON_IsString(v) && v->valuestring) snprintf(out, outsz, "%s", v->valuestring);
}

int render_parse(const cJSON *body, const cJSON *sam, render_req *req, render_error *err)
{
    str_field(body, sam, "input", req->input, sizeof req->input);
    str_field(body, sam, "voice", req->voice, sizeof req->voice);
    str_field(body, sam, "response_format", req->response_format, sizeof req->response_format);
    str_field(body, sam, "input_type", req->input_type, sizeof req->input_type);
    str_field(body, sam, "effect", req->effect, sizeof req->effect);

    req->speed = num_field(body, sam, "speed", 1.0);
    req->pitch_semitones = num_field(body, sam, "pitch_semitones", 0.0);
    req->volume = (int)lround(num_field(body, sam, "volume", 100.0));
    req->word_timestamps = bool_field(body, sam, "word_timestamps", 0);

    const cJSON *rate_item = field(body, sam, "rate");
    req->have_rate = (rate_item && cJSON_IsNumber(rate_item)) ? 1 : 0;
    req->raw_rate = req->have_rate ? (int)lround(rate_item->valuedouble) : 0;

    if (!*req->input) {
        fail(err, 400, "missing_input", "input is required");
        return 0;
    }
    if (req->speed < SPEED_MIN) req->speed = SPEED_MIN;
    if (req->speed > SPEED_MAX) req->speed = SPEED_MAX;
    if (req->pitch_semitones < PITCH_MIN) req->pitch_semitones = PITCH_MIN;
    if (req->pitch_semitones > PITCH_MAX) req->pitch_semitones = PITCH_MAX;
    if (req->volume < VOLUME_MIN) req->volume = VOLUME_MIN;
    if (req->volume > VOLUME_MAX) req->volume = VOLUME_MAX;
    return 1;
}

/* ----------------------------------------------------------------- render */

int render_speech(const char *data_dir, const render_req *req, render_out *out, render_error *err)
{
    char engine_err[256] = "";
    sam_speech *t = sam_tts_open(data_dir, req->voice, engine_err, sizeof engine_err);
    if (!t) {
        char m[sizeof err->message];
        snprintf(m, sizeof m, "engine could not open voice: %s", engine_err);
        fail(err, 500, "engine_open_failed", m);
        return -1;
    }

    /* speed and rate are two views of one control: rate is the exponent of the
     * 3^(rate/10) duration divisor, so speed 3.0 is rate 10 and half speed is -10 */
    int rate = req->have_rate ? req->raw_rate : (int)lround(10.0 * log(req->speed) / log(3.0));
    if (rate < RATE_MIN) rate = RATE_MIN;
    if (rate > RATE_MAX) rate = RATE_MAX;
    sam_tts_set_rate(t, rate);
    sam_tts_set_volume(t, req->volume);
    sam_tts_set_pitch(t, (int)lround(req->pitch_semitones * 2.0));

    int effect_rc = 0;
    if (*req->effect && strcmp(req->effect, "none") != 0)
        effect_rc = sam_tts_set_effect(t, req->effect);

    int sr = sam_tts_sample_rate(t);
    render_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.pcm.cap = req->max_pcm_bytes;
    ctx.text = req->input;
    ctx.rate_hz = sr;
    if (req->word_timestamps) ctx.word = calloc(MAX_WORDS, sizeof(word_t));
    sam_callbacks cb = {pcm_cb, (req->word_timestamps && ctx.word) ? event_cb : NULL, &ctx};

    int flags = (!strcmp(req->input_type, "sapi-xml") || !strcmp(req->input_type, "sapi5-xml"))
                    ? SAM_SPEAK_XML
                    : 0;
    int rc = sam_tts_speak(t, req->input, flags, &cb);
    sam_tts_close(t);

    if (rc < 0) {
        free(ctx.word);
        free(ctx.pcm.pcm);
        fail(err, 400, "speak_failed", "the engine rejected this text");
        return -1;
    }
    if (ctx.pcm.overflow) {
        free(ctx.word);
        free(ctx.pcm.pcm);
        fail(err, 413, "duration_cap", "rendered audio exceeds the configured cap");
        return -1;
    }

    out->audio = ctx.pcm.pcm;
    out->audio_len = ctx.pcm.len;
    out->sample_rate = sr;
    out->rate = rate;
    out->speed = pow(3.0, rate / 10.0);
    out->pitch_semitones = req->pitch_semitones;
    out->volume = req->volume;
    out->seconds = (double)ctx.pcm.len / 2.0 / (double)sr;
    snprintf(out->voice, sizeof out->voice, "%s", req->voice);
    out->effect_ignored = (effect_rc != 0 && *req->effect) ? 1 : 0;
    out->nwords = ctx.nwords;
    if (ctx.word) {
        out->words = calloc((size_t)ctx.nwords, sizeof(render_word));
        if (!out->words) out->nwords = 0;
        for (int i = 0; i < ctx.nwords && out->words; i++) {
            double end = (i + 1 < ctx.nwords) ? ctx.word[i + 1].start : out->seconds;
            snprintf(out->words[i].text, sizeof out->words[i].text, "%s", ctx.word[i].w);
            out->words[i].start = ctx.word[i].start;
            out->words[i].end = end;
        }
        free(ctx.word);
    }
    return 0;
}

/* --------------------------------------------------------- response bodies */

unsigned char *render_wav(const render_out *out, size_t *len)
{
    unsigned char *wav = malloc(44 + out->audio_len);
    if (!wav) return NULL;
    wav_header(wav, (uint32_t)out->sample_rate, (uint32_t)out->audio_len);
    memcpy(wav + 44, out->audio, out->audio_len);
    *len = 44 + out->audio_len;
    return wav;
}

char *render_metadata_json(const render_out *out)
{
    cJSON *meta = cJSON_CreateObject();
    if (!meta) return NULL;
    cJSON_AddNumberToObject(meta, "seconds", out->seconds);
    cJSON_AddNumberToObject(meta, "sample_rate", out->sample_rate);
    cJSON_AddNumberToObject(meta, "rate", out->rate);
    cJSON_AddNumberToObject(meta, "speed", out->speed);
    cJSON_AddNumberToObject(meta, "pitch_semitones", out->pitch_semitones);
    cJSON_AddNumberToObject(meta, "volume", out->volume);
    cJSON_AddStringToObject(meta, "voice", out->voice);
    cJSON_AddNumberToObject(meta, "bytes", (double)out->audio_len);
    cJSON *wl = cJSON_AddArrayToObject(meta, "words");
    for (int i = 0; i < out->nwords; i++) {
        cJSON *w = cJSON_CreateObject();
        if (!w) continue;
        cJSON_AddStringToObject(w, "word", out->words[i].text);
        cJSON_AddNumberToObject(w, "start", out->words[i].start);
        cJSON_AddNumberToObject(w, "end", out->words[i].end);
        cJSON_AddItemToArray(wl, w);
    }
    char *text = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    return text;
}

void render_out_free(render_out *out)
{
    free(out->audio);
    free(out->words);
    out->audio = NULL;
    out->words = NULL;
    out->nwords = 0;
    out->audio_len = 0;
}
