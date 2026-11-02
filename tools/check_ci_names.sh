#!/usr/bin/env bash
# Workflow naming gate. A run step without a name shows its command line in the UI, and
# a lowercase step name reads as an identifier rather than a label. Job keys and matrix
# values stay identifier-shaped; the name fields are what humans read.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

shopt -s nullglob
files=(.github/workflows/*.yml .github/actions/*/*.yml)
if ((${#files[@]} == 0)); then
    printf 'no workflow files found\n' >&2
    exit 1
fi

python3 - "${files[@]}" <<'PY'
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("pyyaml is required to check workflow naming")

bad = []
for path in sys.argv[1:]:
    with open(path) as handle:
        doc = yaml.safe_load(handle)
    if not isinstance(doc, dict):
        continue
    steps = list(doc.get("steps") or [])
    for job_id, job in (doc.get("jobs") or {}).items():
        if not isinstance(job, dict):
            continue
        if not re.fullmatch(r"[A-Za-z0-9_-]+", str(job_id)):
            bad.append(f"{path}: job id {job_id!r} is not identifier-shaped")
        steps.extend(job.get("steps") or [])
    for step in steps:
        if not isinstance(step, dict):
            continue
        name = step.get("name")
        if name is None:
            # a `uses:` step displays the action name, which is already readable
            if "uses" not in step:
                bad.append(f"{path}: run step has no name: {str(step.get('run'))[:40]!r}")
            continue
        if "${{" in name:
            bad.append(f"{path}: step name interpolates a value: {name!r}")
        elif not name[0].isupper():
            bad.append(f"{path}: step name starts lowercase: {name!r}")

if bad:
    print("\n".join(bad))
    sys.exit(1)
print("workflow steps are named")
PY
