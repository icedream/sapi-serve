#!/usr/bin/env bash
# Regression gate: the portable C port must stay within one quantisation step of audio
# rendered by Microsoft's own engine.
#
# This needs a reference corpus, and the repository cannot ship one: the WAVs are derived
# from Microsoft's voice data, exactly like the .spd/.sdf/.LXA files. Produce it once on a
# licensed install that runs the real SAPI runtime, rendering tools/corpus.txt line by line
# with one process per line, then point SAM_REF at that directory:
#
#   SAM_REF=/path/to/reference ./tools/golden_test.sh
#
# The reference has to use the same lexicon the port ships. Windows XP's installed
# LTTS1033.LXA and the Speech SDK 5.1 copy differ for a handful of words, and a line that
# hits one of them comes out with a different duration for a reason that has nothing to do
# with the port. Rendering the corpus twice, once with each lexicon, makes that visible.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
ref="${SAM_REF:?no reference corpus: set SAM_REF to a directory of NN.wav files rendered from tools/corpus.txt}"

if [[ ! -d $ref ]]; then
  echo "no reference corpus at $ref" >&2
  echo "render tools/corpus.txt on a licensed SAPI install, then re-run this" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "rendering tools/corpus.txt with the port..."
SAM_OUT="$work" "$root/tools/render_port.sh" >/dev/null

echo "comparing against $ref:"
python3 "$root/tools/golden_check.py" "$work" "$ref" "${1:-1}" "${2:-0.5}"
echo "port matches the reference within ${1:-1} LSB on every line"
