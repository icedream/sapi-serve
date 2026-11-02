#!/usr/bin/env python3
"""Compare a Linux-port WAV against a Windows XP reference WAV sample-by-sample."""
import array
import sys


def load(path, hdr):
    with open(path, "rb") as f:
        f.seek(hdr)
        a = array.array("h")
        a.frombytes(f.read())
    return a


def analyse(lin, xp):
    n = min(len(lin), len(xp))
    first = next((i for i in range(n) if lin[i] != xp[i]), None)
    if first is None:
        return dict(identical=True, n=n)

    diffs = [i for i in range(n) if lin[i] != xp[i]]
    ad = [abs(lin[i] - xp[i]) for i in diffs]
    ratio = [xp[i] / lin[i] for i in diffs[:200] if lin[i] not in (0,)]

    # small-shift alignment test
    best_shift, best_match = 0, 0
    for s in range(-8, 9):
        m = sum(1 for i in range(max(0, -s), min(n, n - s)) if lin[i] == xp[i + s])
        if m > best_match:
            best_shift, best_match = s, m

    return dict(
        identical=False, n=n, ndiff=len(diffs), pct=100.0 * len(diffs) / n,
        first=first, first_sec=first / 22050.0,
        maxabs=max(ad), meanabs=sum(ad) / len(ad),
        ratio_mean=sum(ratio) / len(ratio) if ratio else None,
        shift=best_shift, shift_match_pct=100.0 * best_match / n,
    )


if __name__ == "__main__":
    a, b = sys.argv[1], sys.argv[2]
    ha = int(sys.argv[3]) if len(sys.argv) > 3 else 44
    hb = int(sys.argv[4]) if len(sys.argv) > 4 else 46
    x, y = load(a, ha), load(b, hb)
    r = analyse(x, y)
    print(f"{a} ({len(x)} samples) vs {b} ({len(y)} samples)")
    if r.get("identical"):
        print("  IDENTICAL sample-for-sample")
    else:
        print(f"  differing samples : {r['ndiff']} of {r['n']} ({r['pct']:.2f}%)")
        print(f"  first difference  : sample {r['first']} ({r['first_sec']*1000:.1f} ms)")
        print(f"  |delta| max/mean  : {r['maxabs']} / {r['meanabs']:.1f}")
        print(f"  xp/linux ratio    : {r['ratio_mean']:.4f} (first 200 diffs)" if r['ratio_mean'] else "  ratio: n/a")
        print(f"  best shift        : {r['shift']} samples ({r['shift_match_pct']:.2f}% match)")
        print(f"  first 6 diffs     : " + ", ".join(
            f"i={i}:{x[i]}->{y[i]}" for i in [j for j in range(r['first'], min(r['first'] + 60, r['n'])) if x[j] != y[j]][:6]))
