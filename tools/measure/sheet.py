# Contact sheets of a batch's renders: the reference's beside MillSaph's, to look at.
#
#   python3 tools/measure/sheet.py <jobs.json> <sheet.png> [job ...] [--scale k] [--crop]
#
# Per job: reference | MillSaph, scaled by the reference's peak, gamma 1 / 2.2, y as rendered;
# a sequence: its frames in a row, the reference's row above MillSaph's. --scale k: every
# k-th pixel (default: 2, 3 for sequences); --crop: around what either shows. PNG (no
# library: zlib).
import json
import os
import struct
import sys
import zlib

import numpy as np

sys.dont_write_bytecode = True  # (no __pycache__ in the repo)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exr  # noqa: E402
from scores import frame_names  # noqa: E402


def write_png(path, rgb):
    """rgb: floats 0..1, [height, width, 3]."""
    img = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def pad_to(rows, value=0.0):
    width = max(r.shape[1] for r in rows)
    return [np.pad(r, ((0, 0), (0, width - r.shape[1]), (0, 0)), constant_values=value)
            for r in rows]


def main(argv):
    if len(argv) < 2 or argv[0] in ("-h", "--help"):
        print(open(__file__).read().split("\nimport")[0])
        return 2
    scale = int(argv[argv.index("--scale") + 1]) if "--scale" in argv else None
    crop = "--crop" in argv
    rest = [a for i, a in enumerate(argv)
            if not a.startswith("--") and (i == 0 or argv[i - 1] != "--scale")]
    jobs = json.load(open(os.path.expanduser(rest[0])))
    out = os.path.expanduser(jobs["out"])
    wanted = set(rest[2:])
    rows = []
    for job in jobs["jobs"]:
        if wanted and job["name"] not in wanted:
            continue
        names = frame_names(job)
        paths = [(os.path.join(out, "reference", n), os.path.join(out, "millsaph", n)) for n in names]
        if not all(os.path.exists(a) and os.path.exists(b) for a, b in paths):
            continue
        refs = [exr.read(a)[..., :3].astype(np.float64) for a, _ in paths]
        mss = [exr.read(b)[..., :3].astype(np.float64) for _, b in paths]
        peak = max(max(np.percentile(r, 99.95) for r in refs), 1e-6)
        if crop:  # around what either shows, in every frame
            shown = sum((r + m).sum(2) for r, m in zip(refs, mss))
            ys, xs = np.nonzero(shown > 1e-3 * shown.max()) if shown.max() > 0 else ([0], [0])
            y0, y1, x0, x1 = max(min(ys) - 8, 0), max(ys) + 9, max(min(xs) - 8, 0), max(xs) + 9
            refs = [r[y0:y1, x0:x1] for r in refs]
            mss = [m[y0:y1, x0:x1] for m in mss]
        k = scale or (3 if len(names) > 1 else 2)
        line = np.ones((refs[0][::k, ::k].shape[0], 2, 3))
        if len(names) == 1:
            rows.append(np.concatenate([refs[0][::k, ::k], line, mss[0][::k, ::k]], 1) / peak)
        else:
            for frames in (refs, mss):
                rows.append(np.concatenate(
                    [np.concatenate([f[::k, ::k], line], 1) for f in frames], 1) / peak)
        rows.append(np.zeros((4, rows[-1].shape[1], 3)))
    if not rows:
        print("no renders to show")
        return 1
    write_png(rest[1], np.clip(np.concatenate(pad_to(rows), 0), 0, None) ** (1 / 2.2))
    print("%s: %d rows" % (rest[1], len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
