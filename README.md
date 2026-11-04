# sapi-serve

Microsoft's SAPI 5 text-to-speech voices (Sam, Mike, Mary) behind an HTTP endpoint, on Linux,
without Windows. The server is a thin C front end over a reimplementation of the SAPI 5 engine.

> **You supply the voice data.** `Sam.spd`, `Sam.sdf` and the lexicons are Microsoft's files.
> They are not in this repository, not in the Docker image, and not in any build artifact. The
> server starts and serves `/v1/models` without them and renders nothing.
> [NOTICE](NOTICE) says where they come from and how to check what you downloaded.

## Quickstart

```sh
git clone --recursive https://github.com/icedream/sapi-serve
cd sapi-serve
make                        # the engine, then the server

# put the voice files where the server looks for them
mkdir -p third_party/ms-sam-mike-mary-decomp/data/voice
cp /path/to/{Sam,Mike,Mary}.{spd,sdf} third_party/ms-sam-mike-mary-decomp/data/voice/
cp /path/to/LTTS1033.LXA third_party/ms-sam-mike-mary-decomp/data/voice/

SAM_DATA_DIR=third_party/ms-sam-mike-mary-decomp/data/voice ./serve/sapi_serve 8080
```

```sh
curl -s localhost:8080/v1/models | python3 -m json.tool

curl -s -o hello.wav -d '{"voice":"Sam","input":"Hello, my name is Microsoft Sam."}' \
  localhost:8080/v1/audio/speech
```

Requirements: a C compiler, `libmicrohttpd` and `cJSON` (Debian/Ubuntu:
`apt install libmicrohttpd-dev libcjson-dev`; Alpine: `apk add libmicrohttpd-dev cjson-dev`).

The root Makefile forwards to the rest: `make test` runs the suite with no voice files
present, `make test-voice` adds the render checks when `SAM_DATA_DIR` points at a voice pack,
and `make style tidy fuzz docker` are the gates CI runs.

## Container

```sh
docker build -t ghcr.io/icedream/sapi-serve:latest .
docker run -d -p 8080:8080 --read-only --cap-drop ALL \
  -v /path/to/voice:/voice:ro ghcr.io/icedream/sapi-serve:latest
```

The image carries no voice data; the mount above is what makes it usable. Published images are
signed with cosign keyless and carry build provenance:

```sh
cosign verify ghcr.io/icedream/sapi-serve:0.1.0 \
  --certificate-identity-regexp 'https://github.com/icedream/sapi-serve/\.github/workflows/release\.yml@.*' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com'
```

## API

`POST /v1/audio/speech` follows the OpenAI shape, so an existing TTS client mostly works.
Sam-specific controls are accepted at the top level and under `sam`, which wins on conflict:

```json
{
  "model": "sapi5-Sam",
  "voice": "sam",
  "input": "Hello, my name is Microsoft Sam.",
  "speed": 1.0,
  "sam": { "rate": 0, "pitch_semitones": 0, "volume": 100, "effect": "none",
           "word_timestamps": true, "response_format": "json" }
}
```

| Route | |
| --- | --- |
| `GET /healthz` | liveness, never requires a token |
| `GET /ready` | readiness: 503 when no voice could be opened, never requires a token |
| `GET /v1/models`, `GET /v1/voices` | voices found on disk, with base pitch and capabilities |
| `POST /v1/audio/speech` | WAV (22050 Hz, 16-bit mono) or `response_format: "json"` |

Full contract, the applied-value response headers and every error code: [docs/api.md](docs/api.md).

## Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `SAM_DATA_DIR` | `data/voice` | directory holding `<Voice>.spd` and `<Voice>.sdf` |
| `SAM_PORT` | `8080` | listen port; also accepted as the first argument |
| `SAM_BIND` | all interfaces | bind address, e.g. `127.0.0.1` |
| `SAM_TOKEN` | none | bearer token for the API routes. Without it every route is open and the server logs a warning |
| `SAM_MAX_SECONDS` | `300` | rejects a render whose audio would be longer |
| `SAM_MAX_HANDLES` | derived | concurrent engine handles; derived from the cgroup memory limit unless set |

TLS and rate limiting come from the reverse proxy in front of it. The server is HTTP only.

`./serve/sapi_serve --version` prints the server version and the engine commit the binary was
built against, which is the pair that makes a rendered sample reproducible.

## Kubernetes

`serve/k8s.yaml` is a Deployment, Service and the voice-pack claim. It runs
`readOnlyRootFilesystem` with all capabilities dropped. `replicas > 1` needs `ReadWriteMany`
for the voice volume; with an RWO volume set `replicas: 1` or let each node fetch its own copy.

## What CI proves

- `ci.yml`: builds under gcc, clang and musl, clang-tidy, clang-format, shellcheck, then the
  request contract against an empty data dir (routing, framing, error codes, token rules). It
  never reaches the engine.
- `regression.yml`: downloads Microsoft's Speech SDK, verifies its hash, renders
  `tools/corpus.txt` and compares against `tools/port_hashes.txt`. That gate detects a change
  in what the port produces. It is not a claim about matching Windows, because a hash cannot
  express a tolerance.
- Equivalence with audio from a real SAPI install is a manual step: `SAM_REF=/path/to/reference
  ./tools/golden_test.sh`. On the corpus the port differs from Windows XP output in the last
  bit of about three in a thousand samples. `tools/golden_test.sh` compares against a
  reference corpus you produce yourself.

## Engine

`third_party/ms-sam-mike-mary-decomp` is a git submodule: a reimplementation from disassembly
of `spttseng.dll`, for interoperability. Its code is the maintainer's; a few small lookup
tables were transcribed from the engine, as upstream states. MIT. Provenance, the
pinned commit and the bump policy are in [NOTICE](NOTICE).

## License

MIT for this repository's code. The voice data and the original SAPI binaries are Microsoft's
and are not distributed here. This project is not affiliated with Microsoft.
