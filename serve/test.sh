#!/bin/sh
# Tests for sapi_serve.
#
# Default mode needs no voice data. Start the server with an empty SAM_DATA_DIR and
# everything below still covers routing, HTTP framing, error codes and the token
# rules. This is the mode public CI runs.
#
#   mkdir -p /tmp/no-voice && SAM_DATA_DIR=/tmp/no-voice ./sapi_serve 18080 &
#   ./test.sh http://localhost:18080
#
# --voice adds the checks that render audio; the server then needs a real SAM_DATA_DIR:
#
#   SAM_DATA_DIR=../third_party/ms-sam-mike-mary-decomp/data/voice ./sapi_serve 18080 &
#   ./test.sh --voice http://localhost:18080
#
# Bearer-token cases run against a second instance started with a token:
#
#   SAM_TOKEN=$TOK SAM_BIND=127.0.0.1 SAM_DATA_DIR=... ./sapi_serve 18081 &
#   SAM_TEST_TOKEN=$TOK ./test.sh --voice http://localhost:18080 http://localhost:18081
voice=0
if [ "$1" = "--voice" ]; then voice=1; shift; fi
base=${1:-http://localhost:18080}
authbase=${2:-}
tmp=$(mktemp -d)
fail=0
check() { if [ "$1" = "$2" ]; then echo "ok      $3"; else echo "FAIL    $3 (got $1, want $2)"; fail=$((fail + 1)); fi; }

# ------------------------------------------------------------------ routing
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/healthz"); check "$code" 200 "healthz"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/v1/models"); check "$code" 200 "models"
if curl -s "$base/v1/models" | grep -q '"voices"'; then
    check yes yes "models body carries a voices array"
else
    check no yes "models body carries a voices array"
fi

# /ready reports the startup engine probe: 200 with a usable voice on disk, 503 without one
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/ready")
if [ "$voice" = 1 ]; then
    check "$code" 200 "ready"
else
    case "$code" in 200|503) check yes yes "ready answers 200 or 503 (got $code)";; *) check "$code" "200|503" "ready";; esac
fi

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/nope");   check "$code" 404 "unknown endpoint is 404"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$base/healthz");  check "$code" 405 "POST to a GET route is 405"
code=$(curl -s -o /dev/null -w '%{http_code}' -X GET "$base/v1/audio/speech"); check "$code" 405 "GET to a POST route is 405"
code=$(curl -s -o /dev/null -w '%{http_code}' -d '{"input":""}' "$base/v1/audio/speech"); check "$code" 400 "empty input is 400"

# ---------------------------------------------------------------- transport
# Framing: both framings must reach the same route with the same parsed body. A
# mis-assembled chunked body fails JSON parsing and changes the status, which is what the
# comparison catches. These hold with no voices on disk.
transport=$(python3 - "$base" <<'PY'
import socket, sys, urllib.parse

u = urllib.parse.urlparse(sys.argv[1])
host, port = u.hostname, u.port

def conn():
    return socket.create_connection((host, port), timeout=15)

def talk(payload):
    s = conn()
    s.sendall(payload)
    data = b""
    while True:
        r = s.recv(65536)
        if not r:
            break
        data += r
    s.close()
    return data

body = b'{"voice":"Sam","input":"Hello world"}'
plain = talk(b"POST /v1/audio/speech HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
             b"Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))

parts = [b'{"voice":"Sam","input":"He', b'llo world"}']
wire = b"".join(b"%x\r\n%s\r\n" % (len(p), p) for p in parts) + b"0\r\n\r\n"
chunked = talk(b"POST /v1/audio/speech HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
               b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + wire)

def status(d):
    return d.split(b"\r\n", 1)[0]

# payloads only: libmicrohttpd's Date: header can differ across a second boundary
def payload(d):
    return d.split(b"\r\n\r\n", 1)[-1]

# what is under test is the framing, not voice availability: both framings must reach
# the same route with the same parsed body, so status and payload must agree
ok = status(plain) == status(chunked) and payload(plain) == payload(chunked)
print(("ok" if ok else "FAIL") + "    chunked body parses like the Content-Length body")

# a body that contains a header-looking line must not end the request: truncating it there
# makes cJSON report the body as malformed (400)
tricky = b'{"voice":"Sam","input":"Content-Length: 999999"}'
t = talk(b"POST /v1/audio/speech HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
         b"Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(tricky), tricky))
ok = status(t) == status(plain) and b"invalid_json" not in payload(t)
print(("ok" if ok else "FAIL") + "    body containing 'Content-Length:' is not treated as a header")

s = conn()
s.sendall(b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")
first = s.recv(4096)
s.sendall(b"GET /ready HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
second = s.recv(4096)
s.close()
ok = first.startswith(b"HTTP/1.1 200") and (second.startswith(b"HTTP/1.1 200")
                                            or second.startswith(b"HTTP/1.1 503"))
print(("ok" if ok else "FAIL") + "    keep-alive serves a second request on one connection")

big = b'{"voice":"Sam","input":"' + b"x" * (600 * 1024) + b'"}'
over = talk(b"POST /v1/audio/speech HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            b"Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(big), big))
print(("ok" if over.startswith(b"HTTP/1.1 413") else "FAIL") + "    oversized body is 413")

bad = talk(b'POST /v1/audio/speech HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n'
           b'Content-Length: 5\r\nConnection: close\r\n\r\n{not json')
print(("ok" if bad.startswith(b"HTTP/1.1 400") else "FAIL") + "    malformed JSON is 400")
PY
)
printf '%s\n' "$transport"
fail=$((fail + $(printf '%s\n' "$transport" | grep -c '^FAIL')))

# -------------------------------------------------------------------- auth
# The probes must stay reachable without a token: a kubelet that gets 401 from /healthz
# restarts a healthy pod.
if [ -n "$authbase" ] && [ -n "$SAM_TEST_TOKEN" ]; then
    code=$(curl -s -o /dev/null -w '%{http_code}' "$authbase/healthz")
    check "$code" 200 "healthz needs no token"
    code=$(curl -s -o /dev/null -w '%{http_code}' "$authbase/ready")
    case "$code" in 200|503) check yes yes "ready needs no token (got $code)";; *) check "$code" "200|503" "ready needs no token";; esac
    code=$(curl -s -o /dev/null -w '%{http_code}' "$authbase/v1/models")
    check "$code" 401 "models without a token is 401"
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $SAM_TEST_TOKEN" "$authbase/v1/models")
    check "$code" 200 "models with the token is 200"
    code=$(curl -s -o /dev/null -w '%{http_code}' -d '{"voice":"Sam","input":"x"}' "$authbase/v1/audio/speech")
    check "$code" 401 "speech without a token is 401"
    code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer wrong-token' -d '{"voice":"Sam","input":"x"}' "$authbase/v1/audio/speech")
    check "$code" 401 "speech with a wrong token is 401"
    if [ "$voice" = 1 ]; then
        code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $SAM_TEST_TOKEN" \
               -d '{"voice":"sam","input":"authed speech"}' "$authbase/v1/audio/speech")
        check "$code" 200 "speech with the token is 200"
    fi
else
    echo "note    token cases skipped: pass a second base URL and SAM_TEST_TOKEN"
fi

# ------------------------------------------------------------ voice checks
if [ "$voice" != 1 ]; then
    echo "note    render checks skipped: pass --voice with a server that has voices on disk"
else
    code=$(curl -s -o "$tmp/a.wav" -w '%{http_code}' -d '{"voice":"Sam","input":"Hello, my name is Microsoft Sam."}' "$base/v1/audio/speech")
    check "$code" 200 "speech returns 200"
    if head -c 4 "$tmp/a.wav" | grep -q RIFF; then
        check yes yes "speech body is a RIFF/WAVE"
    else
        check no yes "speech body is a RIFF/WAVE"
    fi
    sr=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$tmp/a.wav" 2>/dev/null)
    check "$sr" 22050 "sample rate 22050"

    n=$(curl -s "$base/v1/models" | grep -o '"base_pitch_hz"' | wc -l)
    if [ "$n" -ge 1 ]; then
        check yes yes "models reports base pitch for each voice ($n)"
    else
        check no yes "models reports base pitch for each voice"
    fi

    # the request voice is matched case-insensitively but opened by its on-disk name
    code=$(curl -s -o "$tmp/lc.wav" -w '%{http_code}' -d '{"voice":"sam","input":"lower case voice name"}' "$base/v1/audio/speech")
    check "$code" 200 "lowercase voice name is accepted"

    code=$(curl -s -o /dev/null -w '%{http_code}' -d '{"voice":"Nobody","input":"x"}' "$base/v1/audio/speech")
    check "$code" 404 "unknown voice is 404"

    # rate law: +10 is 3x faster, so the audio should be about 3x shorter
    long=$(curl -s -d '{"voice":"Sam","input":"The quick brown fox jumps over the lazy dog.","response_format":"json"}' "$base/v1/audio/speech" | grep -o '"seconds":[0-9.]*' | cut -d: -f2)
    fast=$(curl -s -d '{"voice":"Sam","input":"The quick brown fox jumps over the lazy dog.","rate":10,"response_format":"json"}' "$base/v1/audio/speech" | grep -o '"seconds":[0-9.]*' | cut -d: -f2)
    ratio=$(python3 -c "print('%.2f' % ($long / $fast))")
    case "$ratio" in 2.9*|3.0*|3.1*) check yes yes "rate +10 is about 3x faster (ratio $ratio)";; *) check "$ratio" 3.0 "rate +10 is about 3x faster";; esac

    code=$(curl -s -o "$tmp/b.wav" -w '%{http_code}' -d '{"voice":"Mike","input":"Mike in the hall.","effect":"hall"}' "$base/v1/audio/speech")
    check "$code" 200 "effect hall accepted"
    if curl -s -D- -o /dev/null -d '{"voice":"Mike","input":"x","effect":"cathedral"}' "$base/v1/audio/speech" |
        grep -qi "X-Sam-Warning"; then
        check yes yes "unknown effect reports a warning header"
    else
        check no yes "unknown effect reports a warning header"
    fi

    if curl -s -d '{"voice":"Sam","input":"Mary had a little lamb.","response_format":"json","word_timestamps":true}' "$base/v1/audio/speech" |
        grep -q '"word":"lamb"'; then
        check yes yes "word timestamps present"
    else
        check no yes "word timestamps present"
    fi

    # determinism: a fresh handle per request must render identical audio, because the
    # engine's accent prominence advances process-global rand() state across speaks
    d1=$(curl -s -o "$tmp/d1.wav" -w '%{http_code}' -d '{"voice":"Sam","input":"Determinism check, one two three."}' "$base/v1/audio/speech")
    d2=$(curl -s -o "$tmp/d2.wav" -w '%{http_code}' -d '{"voice":"Sam","input":"Determinism check, one two three."}' "$base/v1/audio/speech")
    say="two identical requests are byte-identical (http $d1/$d2, $(wc -c <"$tmp/d1.wav")/$(wc -c <"$tmp/d2.wav") bytes)"
    if cmp -s "$tmp/d1.wav" "$tmp/d2.wav"; then
        check yes yes "$say"
    else
        check no yes "$say"
    fi
fi

rm -rf "$tmp"
[ "$fail" -eq 0 ] && echo "all tests passed" || echo "$fail test(s) failed"
exit $fail
