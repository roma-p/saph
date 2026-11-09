# Compares renders: two EXR files, or two folders (the files with the same name in both).
#
#   python3 tools/measure/compare.py A B [--diff folder] [--channels RGB]
#
# Per file: the largest and mean absolute difference, the largest relative to the reference's
# own range (A's max - min), how many values differ by more than 1e-4, and where the largest
# is (x, y from the top-left, channel). NaN and infinity are compared as values: a NaN on one
# side only counts as a difference of infinity. --diff writes |A - B| per file there (EXR), to
# look at in Nuke. Exit status 0 when every file is within 1e-4 relative, 1 otherwise.
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exr  # noqa: E402


def difference(a, b):
    d = np.abs(a.astype(np.float64) - b.astype(np.float64))
    same = (a == b) | (np.isnan(a) & np.isnan(b))
    d[same] = 0
    d[np.isnan(d)] = np.inf
    return d


def compare(path_a, path_b, chans, diff_dir):
    a, b = exr.read(path_a), exr.read(path_b)
    if a.shape != b.shape:
        return "size %s vs %s" % (a.shape[1::-1], b.shape[1::-1]), False
    a, b = a[:, :, :chans], b[:, :, :chans]
    d = difference(a, b)
    finite = a[np.isfinite(a)]
    span = float(finite.max() - finite.min()) if finite.size else 1.0
    span = span if span > 0 else 1.0
    worst = float(d.max())
    y, x, c = np.unravel_index(int(np.argmax(d)), d.shape)
    if diff_dir:
        os.makedirs(diff_dir, exist_ok=True)
        exr.write(os.path.join(diff_dir, os.path.basename(path_a)),
                  np.where(np.isfinite(d), d, 1e9).astype(np.float32), "RGBA"[:chans])
    text = "max %.3g (%.3g of range) mean %.3g, %d values > 1e-4, worst at x %d y %d %s (%g vs %g)" % (
        worst, worst / span, float(np.mean(np.where(np.isfinite(d), d, 0))),
        int(np.sum(d > 1e-4)), x, y, "RGBA"[c], a[y, x, c], b[y, x, c])
    return text, math.isfinite(worst) and worst / span <= 1e-4


def main():
    args = sys.argv[1:]
    diff_dir, chans = None, 4
    if "--diff" in args:
        i = args.index("--diff")
        diff_dir = args[i + 1]
        del args[i:i + 2]
    if "--channels" in args:
        i = args.index("--channels")
        chans = len(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print("usage: compare.py A B [--diff folder] [--channels RGB]")
        sys.exit(2)
    a, b = args
    if os.path.isdir(a):
        names = sorted(n for n in os.listdir(a) if n.endswith(".exr"))
        pairs = [(n, os.path.join(a, n), os.path.join(b, n)) for n in names]
    else:
        pairs = [(os.path.basename(a), a, b)]
    ok = True
    for name, pa, pb in pairs:
        if not os.path.exists(pb):
            print("%-40s missing in %s" % (name, b))
            ok = False
            continue
        text, good = compare(pa, pb, chans, diff_dir)
        ok &= good
        print("%-40s %s %s" % (name, "ok  " if good else "DIFF", text))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
