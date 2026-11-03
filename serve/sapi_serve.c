/* sam_serve - Microsoft Sam as an HTTP TTS endpoint.
 *
 *   POST /v1/audio/speech   {"model","voice","input","response_format","speed",
 *                            "sam":{"rate","pitch_semitones","volume","effect",
 *                                   "input_type","word_timestamps"}}
 *   GET  /v1/models         voices found in the data dir + capability flags
 *   GET  /healthz           no voice data needed
 *   GET  /ready             one sam_tts_open() at startup, 503 if it failed
 *
 * HTTP framing is GNU libmicrohttpd and JSON is cJSON; this file holds only the Sam
 * semantics. The OpenAI field names are honoured as-is, and everything Sam-specific that
 * OpenAI has no equivalent for lives under the "sam" object.
 *
 * Engine: libsam (the portable C port of spttseng.dll), through src/sam_tts.h.
 * One engine handle per request: the voice takes word-accent prominence from the C
 * runtime rand(), whose state runs on across sam_tts_speak() calls inside a process, so
 * a reused handle would not reproduce its own earlier output. Opening costs 1.7 ms, which
 * buys determinism, request-level caching and idempotency.
 *
 * Authentication is a single bearer token. For TLS put it behind a gateway.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <microhttpd.h>
#include <cjson/cJSON.h>

#include "render.h"
#include "sam_tts.h"

/* ------------------------------------------------------------------ config */

#define MAX_BODY ((size_t)512 * 1024) /* compared against size_t lengths, so size_t */
#define MAX_VOICES 16

/* stamped by the Makefile; a bare cc invocation gets the fallbacks */
#ifndef SAM_VERSION
#define SAM_VERSION "dev"
#endif
#ifndef SAM_ENGINE_COMMIT
#define SAM_ENGINE_COMMIT "unknown"
#endif
#define LOG(fmt, ...) fprintf(stderr, "[sam] " fmt "\n", ##__VA_ARGS__)

static char g_data_dir[512] = "data/voice";
static int g_port = 8080;
static size_t g_max_pcm_bytes = 0; /* duration cap, bytes of 16-bit mono PCM */
static int g_max_handles = 8;      /* in-flight engine handles */
static char g_token[256] = "";
static char g_bind[64] = ""; /* empty means all interfaces, as before */
static atomic_int g_handles_in_use;
static atomic_int g_ready;

typedef struct {
    char name[64];
    int base_pitch;
} voice_t;
static voice_t g_voices[MAX_VOICES];
static int g_nvoices;

/* ------------------------------------------------------------------- voices */

/* Environment and argument parsing with the validation atoi skips: a typo in
 * SAM_PORT should be reported and ignored, not silently turned into port 0. */
static int parse_int(const char *what, const char *text, int *out)
{
    if (!text || !*text) return 0;
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || n < 0 || n > 0x7FFFFFFF) {
        LOG("ignoring %s: not a non-negative integer: %s", what, text);
        return 0;
    }
    *out = (int)n;
    return 1;
}

static void env_str(const char *name, char *out, size_t outsz)
{
    const char *v = getenv(name);
    if (v && *v) snprintf(out, outsz, "%s", v);
}

static void env_int(const char *name, int *out)
{
    const char *v = getenv(name);
    if (v) parse_int(name, v, out);
}

static int base_pitch_of(const char *dir, const char *voice)
{
    char path[512];
    if (snprintf(path, sizeof path, "%s/%s.sdf", dir, voice) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char b[0x31c];
    size_t got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got < sizeof b || memcmp(b, "Vois", 4) != 0) return 0;
    /* little-endian u32 at the F0 field; unsigned so the shifts are not on signed ints */
    uint32_t v = (uint32_t)b[0x318] | ((uint32_t)b[0x319] << 8U) | ((uint32_t)b[0x31a] << 16U) |
                 ((uint32_t)b[0x31b] << 24U);
    return (int)v;
}

static void scan_voices(void)
{
    DIR *d = opendir(g_data_dir);
    if (!d) {
        LOG("cannot open data dir %s: %s", g_data_dir, strerror(errno));
        return;
    }
    struct dirent *e;
    for (;;) {
        e = readdir(d);
        if (!e || g_nvoices >= MAX_VOICES) break;
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".spd") != 0) continue;
        char name[64];
        size_t n = len - 4;
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, e->d_name, n);
        name[n] = 0;
        if (strcmp(name, "LTTS1033") == 0 || strcmp(name, "r1033tts") == 0) continue;
        int bp = base_pitch_of(g_data_dir, name);
        if (bp <= 0) continue; /* a .spd without its .sdf is not a usable voice */
        snprintf(g_voices[g_nvoices].name, sizeof g_voices[0].name, "%s", name);
        g_voices[g_nvoices].base_pitch = bp;
        g_nvoices++;
    }
    closedir(d);
    LOG("voices: %d (%s)", g_nvoices, g_nvoices ? g_voices[0].name : "-");
}

/* Case-insensitive match against the names on disk, and hands back the disk spelling: libsam
 * opens "<voice>.spd" verbatim, so the request's casing must not reach sam_tts_open. */
static int voice_canonical(const char *v, char *out, size_t outsz)
{
    for (int i = 0; i < g_nvoices; i++)
        if (!strcasecmp(g_voices[i].name, v)) {
            size_t n = strlen(g_voices[i].name);
            if (n >= outsz) n = outsz - 1;
            memmove(out, g_voices[i].name, n); /* the caller passes the same buffer in */
            out[n] = 0;
            return 1;
        }
    return 0;
}

/* ------------------------------------------------------------------- auth */

/* The token is a shared secret, so compare without an early exit: a byte-at-a-time strcmp
 * shortcut tells an attacker how many leading characters they guessed. The length check is
 * allowed to leak; a token of the wrong length is not a prefix guess. */
static int token_ok(const char *presented)
{
    if (!presented) return 0;
    if (strncasecmp(presented, "Bearer ", 7) != 0) return 0;
    const char *got = presented + 7;
    size_t want = strlen(g_token);
    size_t have = strlen(got);
    if (have != want) return 0;
    unsigned diff = 0;
    for (size_t i = 0; i < want; i++)
        diff |= (unsigned)(unsigned char)g_token[i] ^ (unsigned)(unsigned char)got[i];
    return diff == 0;
}

/* Liveness and readiness must stay reachable without a token: a kubelet that cannot read them
 * restarts a perfectly healthy pod. */
static int route_needs_auth(const char *url)
{
    return strcmp(url, "/healthz") != 0 && strcmp(url, "/ready") != 0;
}

/* ---------------------------------------------------------------- responses */

static struct MHD_Response *json_response(const char *body, size_t blen, int code)
{
    struct MHD_Response *r =
        MHD_create_response_from_buffer(blen, (void *)body, MHD_RESPMEM_MUST_COPY);
    if (r) MHD_add_response_header(r, "Content-Type", "application/json; charset=utf-8");
    (void)code;
    return r;
}

static enum MHD_Result send_response(struct MHD_Connection *conn, int code, struct MHD_Response *r)
{
    if (!r) return MHD_NO;
    enum MHD_Result rc = MHD_queue_response(conn, (unsigned)code, r);
    MHD_destroy_response(r);
    return rc;
}

static enum MHD_Result send_error(struct MHD_Connection *conn, int code, const char *code_str,
                                  const char *msg)
{
    cJSON *err = cJSON_CreateObject();
    cJSON *inner = cJSON_AddObjectToObject(err, "error");
    cJSON_AddStringToObject(inner, "message", msg);
    cJSON_AddStringToObject(inner, "type",
                            code == 429 ? "rate_limit_error" : "invalid_request_error");
    cJSON_AddStringToObject(inner, "code", code_str);
    char *body = cJSON_PrintUnformatted(err);
    cJSON_Delete(err);
    if (!body) return MHD_NO;
    struct MHD_Response *r = json_response(body, strlen(body), code);
    free(body);
    if (r && code == 429) MHD_add_response_header(r, "Retry-After", "1");
    return send_response(conn, code, r);
}

static enum MHD_Result send_text(struct MHD_Connection *conn, int code, const char *text)
{
    struct MHD_Response *r =
        MHD_create_response_from_buffer(strlen(text), (void *)text, MHD_RESPMEM_MUST_COPY);
    if (r) MHD_add_response_header(r, "Content-Type", "text/plain");
    return send_response(conn, code, r);
}

/* ---------------------------------------------------------------- capabilities */

static enum MHD_Result handle_models(struct MHD_Connection *conn)
{
    static const char *effects[] = {
        "none",      "hall",      "stadium",   "space",     "room",      "whisper",   "robot",
        "robosoft1", "robosoft2", "robosoft3", "robosoft4", "robosoft5", "robosoft6", "monotone",
    };

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "object", "list");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    cJSON *model = cJSON_CreateObject();
    cJSON_AddStringToObject(model, "id", "sam-sapi5");
    cJSON_AddStringToObject(model, "object", "model");
    cJSON_AddStringToObject(model, "owned_by", "microsoft");

    cJSON *voices = cJSON_AddArrayToObject(model, "voices");
    for (int i = 0; i < g_nvoices; i++) {
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "id", g_voices[i].name);
        cJSON_AddStringToObject(v, "locale", "en-US");
        cJSON_AddNumberToObject(v, "base_pitch_hz", g_voices[i].base_pitch);
        cJSON_AddItemToArray(voices, v);
    }

    cJSON *caps = cJSON_AddObjectToObject(model, "capabilities");
    cJSON_AddItemToObject(caps, "sample_rates", cJSON_CreateNumber(22050));
    cJSON *fmts = cJSON_AddArrayToObject(caps, "response_formats");
    cJSON_AddItemToArray(fmts, cJSON_CreateString("wav"));
    cJSON_AddItemToArray(fmts, cJSON_CreateString("json"));
    cJSON_AddBoolToObject(caps, "streaming", 0);
    cJSON_AddBoolToObject(caps, "word_timestamps", 1);
    cJSON_AddBoolToObject(caps, "visemes", 0);
    cJSON *itypes = cJSON_AddArrayToObject(caps, "input_types");
    cJSON_AddItemToArray(itypes, cJSON_CreateString("text"));
    cJSON_AddItemToArray(itypes, cJSON_CreateString("sapi-xml"));
    cJSON_AddStringToObject(caps, "ssml", "none");
    cJSON_AddBoolToObject(caps, "phoneme_input", 0);
    cJSON_AddBoolToObject(caps, "user_lexicon", 0);
    cJSON *langs = cJSON_AddArrayToObject(caps, "languages");
    cJSON_AddItemToArray(langs, cJSON_CreateString("en-US"));
    cJSON *speed = cJSON_AddArrayToObject(caps, "speed");
    cJSON_AddItemToArray(speed, cJSON_CreateNumber(0.33));
    cJSON_AddItemToArray(speed, cJSON_CreateNumber(7.22));
    cJSON *pitch = cJSON_AddArrayToObject(caps, "pitch_semitones");
    cJSON_AddItemToArray(pitch, cJSON_CreateNumber(-5));
    cJSON_AddItemToArray(pitch, cJSON_CreateNumber(5));
    cJSON *vol = cJSON_AddArrayToObject(caps, "volume");
    cJSON_AddItemToArray(vol, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(vol, cJSON_CreateNumber(100));
    cJSON *eff = cJSON_AddArrayToObject(caps, "effects");
    for (unsigned i = 0; i < sizeof effects / sizeof *effects; i++)
        cJSON_AddItemToArray(eff, cJSON_CreateString(effects[i]));

    cJSON_AddItemToArray(data, model);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return send_error(conn, 500, "oom", "could not build the model list");
    struct MHD_Response *r = json_response(body, strlen(body), 200);
    free(body);
    return send_response(conn, 200, r);
}

/* ------------------------------------------------------------------ speech */

/* Transport only. Field mapping, clamping, the engine call and the response bytes live
 * in render.c; what stays here is the JSON parse, the voice table lookup, the in-flight
 * handle cap and the HTTP response. */
static enum MHD_Result handle_speech(struct MHD_Connection *conn, const char *body_text)
{
    cJSON *root = cJSON_Parse(body_text);
    if (!root) return send_error(conn, 400, "invalid_json", "the request body is not valid JSON");
    const cJSON *sam = cJSON_GetObjectItemCaseSensitive(root, "sam");

    render_req req;
    render_out out;
    render_error err;
    memset(&req, 0, sizeof req);
    memset(&out, 0, sizeof out);
    req.max_pcm_bytes = g_max_pcm_bytes;
    if (!render_parse(root, sam, &req, &err)) {
        cJSON_Delete(root);
        return send_error(conn, err.status, err.code, err.message);
    }
    cJSON_Delete(root);

    if (!voice_canonical(req.voice, req.voice, sizeof req.voice)) {
        char m[160];
        snprintf(m, sizeof m, "unknown voice '%s' (see /v1/models)", req.voice);
        return send_error(conn, 404, "voice_not_found", m);
    }

    int slot = atomic_fetch_add(&g_handles_in_use, 1);
    if (slot >= g_max_handles) {
        atomic_fetch_sub(&g_handles_in_use, 1);
        return send_error(conn, 429, "busy", "in-flight handle cap reached");
    }
    int rc = render_speech(g_data_dir, &req, &out, &err);
    atomic_fetch_sub(&g_handles_in_use, 1);
    if (rc != 0) return send_error(conn, err.status, err.code, err.message);

    struct MHD_Response *resp = NULL;
    if (strcmp(req.response_format, "json") == 0) {
        char *text = render_metadata_json(&out);
        if (text) {
            resp = json_response(text, strlen(text), 200);
            free(text);
        }
    } else {
        size_t len = 0;
        unsigned char *wav = render_wav(&out, &len);
        if (wav) {
            resp = MHD_create_response_from_buffer(len, wav, MHD_RESPMEM_MUST_FREE);
            if (resp) MHD_add_response_header(resp, "Content-Type", "audio/wav");
        }
    }
    if (!resp) {
        render_out_free(&out);
        return send_error(conn, 500, "oom", "could not allocate the response body");
    }

    /* the values that were applied, not the ones requested: rate, pitch and volume are
     * all clamped somewhere between the request and the engine */
    char n[32];
    snprintf(n, sizeof n, "%d", out.rate);
    MHD_add_response_header(resp, "X-Sam-Rate", n);
    snprintf(n, sizeof n, "%.3f", out.speed);
    MHD_add_response_header(resp, "X-Sam-Speed", n);
    snprintf(n, sizeof n, "%.2f", out.pitch_semitones);
    MHD_add_response_header(resp, "X-Sam-Pitch-Semitones", n);
    snprintf(n, sizeof n, "%d", out.volume);
    MHD_add_response_header(resp, "X-Sam-Volume", n);
    snprintf(n, sizeof n, "%d", out.sample_rate);
    MHD_add_response_header(resp, "X-Sam-Sample-Rate", n);
    snprintf(n, sizeof n, "%.3f", out.seconds);
    MHD_add_response_header(resp, "X-Sam-Seconds", n);
    MHD_add_response_header(resp, "X-Sam-Voice", out.voice);
    if (out.effect_ignored)
        MHD_add_response_header(resp, "X-Sam-Warning", "unknown effect, ignored");

    render_out_free(&out);
    return send_response(conn, 200, resp);
}

/* ------------------------------------------------------- request dispatch */

typedef struct {
    char *body;
    size_t len;
    int too_big;
    int reject; /* unauthenticated: read and discard the body, never buffer it */
} req_buf;

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn, const char *url,
                                  const char *method, const char *version, const char *upload_data,
                                  size_t *upload_data_size, void **con_cls)
{
    (void)cls;
    (void)version;

    /* MHD calls the handler once before the body arrives, then once per chunk. Headers are already
     * parsed at that first call, so the token is checked before any request bytes are buffered. */
    if (*con_cls == NULL) {
        req_buf *fresh = calloc(1, sizeof(req_buf));
        if (!fresh) return MHD_NO;
        if (*g_token && route_needs_auth(url))
            fresh->reject =
                !token_ok(MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization"));
        *con_cls = fresh;
        return MHD_YES;
    }
    req_buf *rb = *con_cls;

    if (*upload_data_size) {
        if (rb->reject) {
            *upload_data_size = 0; /* discard: an unauthenticated peer gets no buffer */
            return MHD_YES;
        }
        if (rb->len + *upload_data_size > MAX_BODY) {
            rb->too_big = 1;
        } else {
            char *grown = realloc(rb->body, rb->len + *upload_data_size + 1);
            if (!grown) return MHD_NO;
            rb->body = grown;
            memcpy(rb->body + rb->len, upload_data, *upload_data_size);
            rb->len += *upload_data_size;
            rb->body[rb->len] = 0;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (rb->reject) return send_error(conn, 401, "unauthorized", "bearer token required");
    if (rb->too_big) return send_error(conn, 413, "payload_too_large", "request body over the cap");

    int get = strcmp(method, "GET") == 0;
    int post = strcmp(method, "POST") == 0;

    if (!strcmp(url, "/healthz"))
        return get ? send_text(conn, 200, "ok\n")
                   : send_error(conn, 405, "method_not_allowed", "use GET");
    if (!strcmp(url, "/ready"))
        return get ? send_text(conn, atomic_load(&g_ready) ? 200 : 503,
                               atomic_load(&g_ready) ? "ready\n" : "no voice data\n")
                   : send_error(conn, 405, "method_not_allowed", "use GET");
    if (!strcmp(url, "/v1/models") || !strcmp(url, "/v1/voices"))
        return get ? handle_models(conn) : send_error(conn, 405, "method_not_allowed", "use GET");
    if (!strcmp(url, "/v1/audio/speech"))
        return post ? handle_speech(conn, rb->body ? rb->body : "")
                    : send_error(conn, 405, "method_not_allowed", "use POST");
    return send_error(conn, 404, "not_found", "no such endpoint");
}

static void request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                              enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)conn;
    (void)toe;
    req_buf *rb = *con_cls;
    if (!rb) return;
    free(rb->body);
    free(rb);
    *con_cls = NULL;
}

/* ---------------------------------------------------------------- cgroup */

static long long cgroup_memory_limit(void)
{
    FILE *f = fopen("/sys/fs/cgroup/memory.max", "r");
    if (f) {
        char b[64];
        long long v = 0;
        if (fgets(b, sizeof b, f) && strncmp(b, "max", 3) != 0) v = strtoll(b, NULL, 10);
        fclose(f);
        return v;
    }
    f = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "r");
    if (f) {
        char b[64];
        long long v = 0;
        if (fgets(b, sizeof b, f)) {
            v = strtoll(b, NULL, 10);
            /* a "no limit" cgroup value arrives as a huge number; unsigned shift so the
             * comparison is not on a signed operand */
            if (v < 0 || v > (long long)(1ULL << 50U)) v = 0;
        }
        fclose(f);
        return v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* the engine commit belongs in the answer: a rendered sample is only reproducible
     * with both halves of the build */
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("sapi_serve %s  engine %s\n", SAM_VERSION, SAM_ENGINE_COMMIT);
        return 0;
    }

    env_str("SAM_DATA_DIR", g_data_dir, sizeof g_data_dir);
    if (argc > 1) parse_int("the port argument", argv[1], &g_port);
    env_int("SAM_PORT", &g_port);
    env_str("SAM_TOKEN", g_token, sizeof g_token);
    env_str("SAM_BIND", g_bind, sizeof g_bind);
    int maxsec = 300;
    env_int("SAM_MAX_SECONDS", &maxsec);
    if (maxsec < 1) maxsec = 1;
    if (g_port < 1 || g_port > 65535) {
        LOG("port out of range: %d", g_port);
        return 1;
    }

    scan_voices();

    /* The in-flight cap comes from the container memory limit: each handle holds a voice's
     * tables plus per-speak state, and its audio buffer grows with SAM_MAX_SECONDS. Leave
     * room for the process itself and the request buffers. */
    long long lim = cgroup_memory_limit();
    g_max_pcm_bytes = (size_t)maxsec * 22050U * 2U;
    if (lim > 0) {
        long long per =
            3145728LL + (long long)g_max_pcm_bytes + 65536; /* handle + audio buffer + headroom */
        long long usable = lim - (long long)(16ULL << 20U);
        if (per < 1) per = 1; /* maxsec is user input; never divide by what it can produce */
        long long n = usable > 0 ? usable / per : 1;
        if (n < 1) n = 1;
        if (n > 512) n = 512;
        g_max_handles = (int)n;
    }
    env_int("SAM_MAX_HANDLES", &g_max_handles);
    if (g_max_handles < 1) g_max_handles = 1;
    LOG("listening on :%d  data=%s  max_seconds=%d  max_handles=%d  cgroup_mem=%lld", g_port,
        g_data_dir, maxsec, g_max_handles, lim);

    if (g_nvoices) {
        char err[256];
        sam_speech *t = sam_tts_open(g_data_dir, g_voices[0].name, err, sizeof err);
        if (t) {
            sam_tts_close(t);
            atomic_store(&g_ready, 1);
        } else {
            LOG("readiness probe failed: %s", err);
        }
    }

    signal(SIGPIPE, SIG_IGN);

    if (!*g_token)
        LOG("no SAM_TOKEN: every endpoint is open. Set SAM_BIND to loopback, or put TLS and rate"
            " limiting in front of it.");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (*g_bind && inet_pton(AF_INET, g_bind, &sa.sin_addr) != 1) {
        LOG("SAM_BIND is not an IPv4 address: %s", g_bind);
        return 1;
    }

    struct MHD_Daemon *d = MHD_start_daemon(
        (unsigned)MHD_USE_INTERNAL_POLLING_THREAD | (unsigned)MHD_USE_THREAD_PER_CONNECTION,
        (uint16_t)g_port, NULL, NULL, &on_request, NULL, MHD_OPTION_SOCK_ADDR,
        (struct sockaddr *)&sa, MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)30,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned)128, MHD_OPTION_NOTIFY_COMPLETED, &request_completed,
        NULL, MHD_OPTION_END);
    if (!d) {
        LOG("cannot listen on %s:%d", *g_bind ? g_bind : "0.0.0.0", g_port);
        return 1;
    }
    LOG("sapi_serve %s engine %s", SAM_VERSION, SAM_ENGINE_COMMIT);
    LOG("listening on %s:%d", *g_bind ? g_bind : "0.0.0.0", g_port);
    LOG("ready");
    for (;;)
        pause();
    MHD_stop_daemon(d);
    return 0;
}
