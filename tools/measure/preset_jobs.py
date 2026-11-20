#!/usr/bin/env python3
# Jobs files rendering every preset with the reference and with MillSaph, to compare them:
#
#   python3 tools/measure/preset_jobs.py [Effect ...]
#
# -> ~/millsaph_measure/jobs/presets_<effect>_reference.json and _millsaph.json (the same jobs,
# one per preset: the reference's node with the preset's settings as its knobs, its styles and
# lenses as the reference's own texts; MillSaph's with MillSaph's preset), rendered into
# ~/millsaph_measure/renders/presets_<effect>/. Then:
#   in Nuke:   nuke_measure.py jobs ~/millsaph_measure/jobs/presets_<effect>_reference.json
#   MillSaph:  python3 tools/measure/ms_render.py ~/millsaph_measure/jobs/presets_<effect>_millsaph.json
#   scores:    python3 tools/measure/scores.py ~/millsaph_measure/jobs/presets_<effect>_millsaph.json
# Knobs in Nuke's units (widths in pixels, choices by name). The picture: MillSaph's test
# picture (thumbnails.cpp's), lens flares on black, written into ~/millsaph_measure/probes_presets.
import json, os, sys

import numpy as np

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import exr  # noqa: E402
import ms_render  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
HOME = os.path.expanduser("~/millsaph_measure")
W, H = 480, 270
EFFECTS = ["BandPass", "EdgeAwareBlur", "Distort", "EdgeBlur", "RackDefocus", "Glow", "Glint",
           "Glare", "LensFlare", "Flicker", "TVDamage", "JpegDamage", "PixelSort", "Mosaic"]
# the reference's text settings: the preset files' key, the knob
TEXTS = {"Glare": ("type", "style_"), "LensFlare": ("lens", "lens_")}


def picture():
    # thumbnails.cpp's test picture (linear RGBA, top row first here)
    v, u = np.mgrid[0:H, 0:W].astype(np.float64)
    u = (u + 0.5) / W
    v = 1.0 - (v + 0.5) / H  # up
    rgb = np.zeros((H, W, 3))
    hill = 0.38 + 0.07 * np.sin(u * 7.0) + 0.04 * np.sin(u * 17.0 + 1.0)
    t = np.clip((v - hill) / (1.0 - hill), 0, 1)
    sky = np.stack([0.55 + (0.12 - 0.55) * t, 0.42 + (0.25 - 0.42) * t, 0.35 + (0.55 - 0.35) * t], -1)
    hsh = np.sin(np.floor(u * 160) * 12.9898 + np.floor(v * 90) * 78.233) * 43758.5453
    n = hsh - np.floor(hsh)
    ground = np.stack([0.07 + 0.05 * n, 0.16 + 0.08 * n, 0.06 + 0.03 * n], -1)
    rgb[:] = np.where((v > hill)[..., None], sky, ground)
    rgb[np.hypot((u - 0.72) * 16 / 9, v - 0.70) < 0.07] = (4.0, 3.2, 2.2)
    rgb[np.hypot((u - 0.22) * 16 / 9, v - 0.30) < 0.12] = (0.75, 0.08, 0.06)
    rgb[(u > 0.40) & (u < 0.55) & (v > 0.12) & (v < 0.36)] = (0.08, 0.22, 0.70)
    rgb[(v > 0.10) & (v < 0.34) & (np.abs(u - 0.86) < (0.34 - v) * 0.5)] = (0.70, 0.56, 0.08)
    panel = (u > 0.05) & (u < 0.17) & (v > 0.55) & (v < 0.85)
    rgb[panel] = np.where(np.sin(u[panel] * 220.0) > 0, 0.62, 0.06)[:, None]
    for k in range(7):
        lx, ly = 0.30 + k * 0.05, 0.50 + 0.02 * (k % 2)
        rgb[np.hypot((u - lx) * W, (v - ly) * H) < 1.6] = (6.0, 5.0, 3.0)
    return np.concatenate([rgb, np.ones((H, W, 1))], -1).astype(np.float32)


def nuke_value(name, v, choices, widths, points):
    # a preset's value (as the OFX parameter holds it) in Nuke's units
    if name in choices and isinstance(v, (int, float)):
        names = {i: label for label, i in choices[name].items()}
        return names.get(int(v), int(v))
    if name in widths and isinstance(v, (int, float)):
        return v * (W if widths[name] == "x" else H)
    if name in points and isinstance(v, list) and len(v) == 2:
        return [v[0] * W, v[1] * H]
    return v


def jobs_for(effect):
    with open(os.path.join(HOME, "presets", effect + ".json"), encoding="utf-8") as f:
        reference = json.load(f)["presets"]
    folder = os.path.join(ROOT, "presets", effect.lower())
    clips, choices, widths = ms_render.describe(effect)
    points = {"center", "circle_center", "hotspot", "pivot"}
    inputs = ["black" if effect == "LensFlare" else "picture"]
    ref_jobs, ms_jobs = [], []
    for f in sorted(os.listdir(folder)):
        if not f.endswith(".json"):
            continue
        with open(os.path.join(folder, f), encoding="utf-8") as fh:
            p = json.load(fh)
        name = os.path.splitext(f)[0]
        ours = {k: nuke_value(k, v, choices, widths, points) for k, v in p["params"].items()}
        theirs = {k: v for k, v in ours.items() if k not in ("style_", "lens_")}
        if effect in TEXTS and p.get("converted"):
            key, knob = TEXTS[effect]
            src = reference.get(p["name"], {}).get("params", {}).get(key)
            if isinstance(src, dict):
                theirs[knob] = "-" * 32 + json.dumps({"type": "flare", "value": src})
        elif effect == "LensFlare":
            continue  # MillSaph's own lenses: the reference crashes on them (nothing to compare)
        elif effect in TEXTS:  # MillSaph's own styles: the same text for both
            key, knob = TEXTS[effect]
            if knob in ours:
                theirs[knob] = ours[knob]
        ref_jobs.append({"name": name, "effect": effect, "inputs": inputs, "knobs": theirs})
        ms_jobs.append({"name": name, "effect": effect, "inputs": inputs, "knobs": ours})
    for vendor, jobs in (("reference", ref_jobs), ("millsaph", ms_jobs)):
        d = {"size": [W, H], "probes": "~/millsaph_measure/probes_presets",
             "out": "~/millsaph_measure/renders/presets_%s" % effect.lower(), "vendors": [vendor],
             "jobs": jobs}
        path = os.path.join(HOME, "jobs", "presets_%s_%s.json" % (effect.lower(), vendor))
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(d, fh, ensure_ascii=False)
    print("%-14s %3d presets" % (effect, len(ms_jobs)))


def main(args):
    probes = os.path.join(HOME, "probes_presets")
    os.makedirs(probes, exist_ok=True)
    exr.write(os.path.join(probes, "picture.exr"), picture())
    black = np.zeros((H, W, 4), np.float32)
    black[..., 3] = 1
    exr.write(os.path.join(probes, "black.exr"), black)
    for e in args or EFFECTS:
        jobs_for(e)


if __name__ == "__main__":
    main(sys.argv[1:])
