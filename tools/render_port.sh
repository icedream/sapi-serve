#!/bin/sh
# Render tools/corpus.txt with the portable C port, one process per line, into out/port/NN.wav.
# One process per line is not a stylistic choice: the engine's accent prominence advances
# process-global rand() state, so a second speak in the same process is not independent of
# the first. The server takes the same rule, opening a handle per request.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
sam="$root/third_party/ms-sam-mike-mary-decomp"
out="${SAM_OUT:-$root/out/port}"
mkdir -p "$out"
n=0
# CRLF-safe: strip \r
tr -d '\r' < "$root/tools/corpus.txt" | while IFS= read -r line; do
    n=$((n + 1))
    printf '%02d ' "$n"
    "$sam/build/sam" --data "$sam/data/voice" "$line" "$out/$(printf '%02d' "$n").wav"
done
echo "wrote $out"
