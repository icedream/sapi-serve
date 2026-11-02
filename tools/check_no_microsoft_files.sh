#!/bin/sh
# Guard for a public repository: the voice data and the SAPI binaries belong to Microsoft
# and must never be tracked. The patterns mirror .gitignore, so the only way to trip this
# is to add one of them deliberately with `git add -f`.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ext='(\.spd|\.sdf|\.lxa|\.LXA|\.exe|\.cab|\.msi|\.wav)$'
names='(spttseng\.dll|sapi\.dll|spcommon\.dll)$'
dirs='(^|/)(data/voice|voice)/'

bad=$( { git ls-files | grep -Ei "$ext"; git ls-files | grep -Ei "$names"; git ls-files | grep -E "$dirs"; } | sort -u || true)
if [ -n "$bad" ]; then
    printf 'Microsoft-owned files are tracked in git:\n%s\n' "$bad" >&2
    printf 'Remove them with git rm --cached; they are mounted at runtime.\n' >&2
    exit 1
fi

tracked_voice=$(git ls-files -- third_party | grep -Ei "$ext" || true)
if [ -n "$tracked_voice" ]; then
    printf 'the vendored engine checkout carries voice data:\n%s\n' "$tracked_voice" >&2
    exit 1
fi

echo "no Microsoft-owned files tracked"
