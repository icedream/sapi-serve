# API

Everything is JSON in, JSON or WAV out. Requests are UTF-8; the body limit is 512 KiB.

## Authentication

`Authorization: Bearer <SAM_TOKEN>` when `SAM_TOKEN` is set. `/healthz` and `/ready` never
require it, so a probe cannot be broken by a token mistake. Without `SAM_TOKEN` every route is
open, which is logged at startup.

## GET /healthz

`200` with `ok`. Reports process liveness only, not whether a voice could be opened.

## GET /ready

`200` with `ready` if the startup engine probe opened a voice, `503` with `not ready` otherwise.
A missing voice pack is a permanent condition until someone mounts files, so `503` here is a
configuration signal, not a transient one.

## GET /v1/models (alias /v1/voices)

Voices discovered in `SAM_DATA_DIR`, in directory order.

```json
{
  "object": "list",
  "data": [
    {
      "id": "sam-sapi5",
      "object": "model",
      "owned_by": "microsoft",
      "voices": [
        { "id": "Mike", "locale": "en-US", "base_pitch_hz": 110 }
      ],
      "capabilities": {
        "sample_rates": 22050,
        "response_formats": ["wav", "json"],
        "streaming": false,
        "word_timestamps": true,
        "visemes": false,
        "input_types": ["text", "sapi-xml", "sapi5-xml"],
        "effects": ["none", "hall", "stadium", "space", "room", "whisper", "robot",
                    "robosoft1", "robosoft2", "robosoft3", "robosoft4", "robosoft5",
                    "robosoft6", "monotone"]
      }
    }
  ]
}
```

`base_pitch_hz` is read from the voice's `.sdf` file, not hard-coded. A `.spd` without its
`.sdf` pair is not listed.

## POST /v1/audio/speech

Keys may sit at the top level (the OpenAI shape) or inside `sam` (the extensions). On conflict
`sam` wins.

| Field | Type | Default | Range | Notes |
| --- | --- | --- | --- | --- |
| `input` | string | required | 16 KiB max | the text to speak |
| `voice` | string | `Sam` | a name from `/v1/models` | matched case-insensitively, opened by its on-disk name |
| `speed` | number | `1.0` | 0.33 to 7.22 | clamped, not rejected |
| `sam.rate` | integer | from `speed` | -10 to 18 | the engine's own control; overrides `speed` |
| `sam.pitch_semitones` | number | `0` | -5 to 5 | applied in half-semitone steps |
| `sam.volume` | integer | `100` | 0 to 100 | |
| `sam.effect` | string | `none` | see `/v1/models` | an unknown name is ignored and reported in `X-Sam-Warning` |
| `sam.input_type` | string | `text` | `text`, `sapi-xml`, `sapi5-xml` | XML input is spoken with the engine's XML parser |
| `sam.response_format` | string | `wav` | `wav`, `json` | |
| `sam.word_timestamps` | boolean | `false` | | only reported with `response_format: "json"` |
| `model` | string | ignored | | accepted so OpenAI clients work |

`speed` and `rate` are two views of one control: duration is divided by `3^(rate/10)`, so
`speed: 3.0` is `rate: 10` and half speed is `rate: -10`.

### Response: WAV

`Content-Type: audio/wav`, 44-byte canonical header, 22050 Hz, 16-bit signed mono PCM. SAPI's
own files carry an 18-byte `fmt` chunk instead of 16, so tools that compare audio should parse
the container rather than assume offsets.

### Response: JSON

```json
{
  "seconds": 1.858,
  "sample_rate": 22050,
  "rate": 0,
  "speed": 1.0,
  "pitch_semitones": 0.0,
  "volume": 100,
  "voice": "Sam",
  "bytes": 81964,
  "words": [
    { "word": "Mary", "start": 0.010, "end": 0.382 }
  ]
}
```

The numbers are the values applied after clamping, which is how a client learns what actually
happened. `words[].end` is the next word's start, or the end of the audio for the last word.

### Response headers

Both response formats carry the same headers, so a client that only wants audio can still read
what was applied:

| Header | |
| --- | --- |
| `X-Sam-Rate` | engine rate applied |
| `X-Sam-Speed` | the speed that rate means |
| `X-Sam-Pitch-Semitones` | as requested, after clamping |
| `X-Sam-Volume` | as requested, after clamping |
| `X-Sam-Sample-Rate` | always 22050 |
| `X-Sam-Seconds` | rendered duration |
| `X-Sam-Voice` | canonical voice name used |
| `X-Sam-Warning` | present when the effect name was not recognised |

### Errors

```json
{ "error": { "message": "unknown voice 'Sammy' (see /v1/models)",
             "type": "invalid_request_error", "code": "voice_not_found" } }
```

| Status | `code` | When |
| --- | --- | --- |
| 400 | `invalid_json` | body did not parse |
| 400 | `missing_input` | `input` absent or empty |
| 400 | `speak_failed` | the engine rejected the text or XML |
| 401 | `unauthorized` | token required and absent or wrong |
| 404 | `voice_not_found` | name not in `/v1/models` |
| 405 | `method_not_allowed` | wrong method for the route |
| 413 | `payload_too_large` | body over 512 KiB |
| 413 | `duration_cap` | render longer than `SAM_MAX_SECONDS` |
| 500 | `engine_open_failed` | voice pair present but unreadable |
| 500 | `oom` | allocation failed |
| 503 | `busy` | `SAM_MAX_HANDLES` concurrent renders already running |

## Determinism

Identical requests produce byte-identical audio. The server opens and closes an engine handle
per request rather than pooling one: the engine's accent prominence advances process-global
`rand()` state across speaks on a single handle, so pooled handles would make output depend on
request order.

## Streaming

Not supported. Chunked *requests* are accepted; responses are complete bodies.
