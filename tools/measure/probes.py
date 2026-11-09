# Writes the probe images of the measurements (tools/measure/README.md) as RGBA float EXR:
#
#   python3 tools/measure/probes.py [folder] [--size WxH]     (default ~/millsaph_measure/probes,
#                                                              256x128)
#
# Each probe isolates one thing an effect may do:
#   impulse      one pixel of 1 (RGB, alpha 1 everywhere) in the middle: a linear filter's kernel
#   impulse_off  the same, at 0.25 + 1 in the middle: does the effect depend on the level
#   step         0.1 | 0.9 halfway across: edge response, symmetry, border handling
#   ramp_x       R = x / width, G = y / height, B = 0.5: level-dependent curves, positions
#   levels       16 vertical bars from 0 to 1.5 (RGB equal): the response to each level
#   colors       8 bars: primaries, secondaries, white, black: channel mixing, luma weights
#   flat         0.5 everywhere: what the effect adds where there is no detail
#   noise        uniform 0..1 per channel (seed 1): general case, statistics
#   alpha_ramp   RGB 0.5, alpha = x / width: alpha use, premultiplication
#   black        0, alpha 1
#   checker      8 px squares 0.1 / 0.9: block and sampling grids (Mosaic, JPEG)
# The image's own top row is the first row of the file (as Nuke shows it).
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exr  # noqa: E402


def probes(w, h):
    x = np.arange(w)[None, :].repeat(h, 0).astype(np.float32)
    y = np.arange(h)[:, None].repeat(w, 1).astype(np.float32)

    def rgba(r, g=None, b=None, a=None):
        g = r if g is None else g
        b = r if b is None else b
        a = np.ones_like(x) if a is None else a
        return np.stack([np.broadcast_to(v, x.shape) for v in (r, g, b, a)], -1)

    impulse = np.zeros_like(x)
    impulse[h // 2, w // 2] = 1
    bars = (np.floor(x * 16 / w) / 15 * 1.5).astype(np.float32)
    pal = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 1, 1], [1, 0, 1], [1, 1, 0],
                    [1, 1, 1], [0, 0, 0]], np.float32)
    col = pal[np.minimum((x * 8 / w).astype(int), 7)]
    rng = np.random.default_rng(1)
    return {
        "impulse": rgba(impulse),
        "impulse_off": rgba(0.25 + impulse),
        "step": rgba(np.where(x < w // 2, 0.1, 0.9).astype(np.float32)),
        "ramp_x": rgba(x / w, y / h, np.full_like(x, 0.5)),
        "levels": rgba(bars),
        "colors": rgba(col[..., 0], col[..., 1], col[..., 2]),
        "flat": rgba(np.full_like(x, 0.5)),
        "noise": np.concatenate([rng.random((h, w, 3), np.float32), np.ones((h, w, 1),
                                                                            np.float32)], -1),
        "alpha_ramp": rgba(np.full_like(x, 0.5), a=x / w),
        "black": rgba(np.zeros_like(x)),
        "checker": rgba(np.where((x // 8 + y // 8) % 2 == 0, 0.1, 0.9).astype(np.float32)),
    }


def main():
    args = sys.argv[1:]
    w, h = 256, 128
    if "--size" in args:
        i = args.index("--size")
        w, h = (int(v) for v in args[i + 1].lower().split("x"))
        del args[i:i + 2]
    folder = os.path.expanduser(args[0] if args else "~/millsaph_measure/probes")
    os.makedirs(folder, exist_ok=True)
    images = probes(w, h)
    for name, image in images.items():
        exr.write(os.path.join(folder, name + ".exr"), image)
    print("%d probes of %d x %d in %s" % (len(images), w, h, folder))


if __name__ == "__main__":
    main()
