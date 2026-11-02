#!/usr/bin/env python3
"""Compare freshly rendered port audio against the Windows XP reference corpus.

Both sides are read through the WAV parser, so the header difference (SAPI writes an 18-byte fmt
chunk, the port a 16-byte one) is not itself a failure. What must hold: identical sample counts, and
every differing sample within one quantisation step.
"""
import array
import sys
import wave
from pathlib import Path


def samples(path):
    with wave.open(str(path), "rb") as w:
        channels, width, rate, frames = w.getparams()[:4]
        data = array.array("h")
        data.frombytes(w.readframes(frames))
    if channels != 1 or width != 2:
        raise SystemExit(f"{path}: expected 16-bit mono, got {width * 8}-bit {channels}ch")
    return data, rate


def check(port_dir: Path, ref_dir: Path, max_delta: int, max_diff_pct: float) -> int:
    failures = []
    for ref in sorted(ref_dir.glob("*.wav")):
        port = port_dir / ref.name
        if not port.exists():
            failures.append(f"{ref.name}: not rendered by the port")
            continue
        a, rate_a = samples(port)
        b, rate_b = samples(ref)
        if rate_a != rate_b:
            failures.append(f"{ref.name}: sample rate {rate_a} vs reference {rate_b}")
            continue
        if len(a) != len(b):
            failures.append(f"{ref.name}: {len(a)} samples vs reference {len(b)}")
            continue
        diff = [i for i in range(len(a)) if a[i] != b[i]]
        delta = max((abs(a[i] - b[i]) for i in diff), default=0)
        pct = 100.0 * len(diff) / len(a)
        status = "ok"
        if delta > max_delta:
            status = f"FAIL delta {delta} > {max_delta}"
        elif pct > max_diff_pct:
            status = f"FAIL {pct:.2f}% differ > {max_diff_pct}%"
        print(f"  {ref.name}  {len(a):>7} samples  {delta} max delta  {pct:>5.2f}% differ  {status}")
        if status != "ok":
            failures.append(f"{ref.name}: {status}")

    if failures:
        print("\n".join(f"FAILED: {f}" for f in failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    port, ref = Path(sys.argv[1]), Path(sys.argv[2])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    pct = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
    sys.exit(check(port, ref, limit, pct))
