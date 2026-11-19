#!/usr/bin/env python3
# MillSaph's presets from the reference's (reference_presets.py's ~/millsaph_measure/presets),
# in MillSaph's settings and units:
#
#   python3 tools/measure/convert_presets.py [Effect ...]     -> presets/<effect>/*.json
#
# One file per preset: {"name": ..., "tags": [...], "converted": true, "params": {name: value}}
# ("converted": written by this script, which replaces only those), values as the
# OFX parameters hold them (widths as fractions of the frame width, choices as indices, colours
# as lists, points as [x, y] fractions, styles and lenses as their JSON text). Applying one:
# every parameter back to its default, then these.
# The conversion (measured by loading presets in the reference, tools/measure/nuke_presets.py):
# widths are the files' value / 8 (a few others, WIDTH_SCALE); some settings were renamed
# (RENAME, SPLIT); some have no effect in a compositing host (DROP); choices are indices into
# the reference's entries, matched to MillSaph's by name. Settings MillSaph does not have yet
# are kept (applying skips them) and reported.
# Needs ~/millsaph_measure/describe/<Effect>.json with both nodes (nuke_measure.py describe).
import json, os, re, sys

HOME = os.path.expanduser("~/millsaph_measure")
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
EFFECTS = ["BandPass", "EdgeAwareBlur", "Distort", "EdgeBlur", "RackDefocus", "Glow", "Glint",
           "Glare", "LensFlare", "Flicker", "FlickerMatch", "FlickerMatchColor", "TVDamage",
           "JpegDamage", "PixelSort", "Mosaic"]
# the settings' names in the files -> the parameters'
RENAME = {
    "BandPass": {"scale": "brightness"},
    "Glow": {"width": "glow_width"},
    "Glare": {"type": "style_"},
    "LensFlare": {"lens": "lens_"},
    "RackDefocus": {"highlight_threshold": "hilight_threshold",
                    "chromatic_aberration": "chroma_distort"},
    "TVDamage": {"ghost_amplitude": "ghost_amp", "interference_amplitude": "interference_amp",
                 "frequency": "interference_frequency", "band_frequency": "FF_band_frequency",
                 "band_height": "FF_band_height", "band_shift": "FF_band_shift"},
}
# [x, y] settings that are two parameters
SPLIT = {"blur_rel": ("blur_rel_x", "blur_rel_y"), "amount_rel": ("amount_rel_x", "amount_rel_y"),
         "dots_speed": ("dots_speed_x", "dots_speed_y"),
         "pixel_shift": ("pixel_shift_x", "pixel_shift_y")}
# no effect in a compositing host (layer opacity, After Effects' background layers, ...)
DROP = {"opacity", "scale_source", "scale_background", "light_bg", "eclipse_with_bg",
        "obsolete_style", "wrap", "iterations"}
# widths: the files' value / 8, except these
WIDTH_SCALE = {"threshold_blur": 1 / 32, "thickness": 1 / 2, "ray_length": 1 / 2,
               "_inner_radius": 1 / 2, "inner_radius": 1 / 2, "flare_width": 1 / 5.5}
WIDTH_MIN = {("EdgeBlur", "edge_width"): 0.001}
WIDTHS = {"blur_amount", "blur_amount1", "blur_amount2", "blur_flare", "blur_glare",
          "blur_glint", "blur_lens", "blur_mask", "blur_width", "defocus_width", "edge_smooth",
          "edge_width", "flare_width", "gauss_blur", "glow_width", "size", "smooth_colors",
          "threshold_blur", "thickness", "ray_length", "_inner_radius", "inner_radius", "blur_",
          "occlusion_softness", "scale_widths"}
# points: offsets from the frame's centre in the files
POINTS = {"center", "circle_center", "hotspot", "pivot"}


# A style's or lens's images (textures, photographs) are not taken: each element's image becomes
# one of MillSaph's generated textures (lensflare_style.c), the kind by the image's file name;
# its seed from that name, so that one image is one texture.
TEXTURES = [("iris", "iris"), ("grad", "ramp"), ("rainbow", "ramp"), ("element", "ramp"),
            ("flare", "ramp"), ("stripe", "ramp"), ("ring", "ramp"), ("glint", "glint"),
            ("ray", "glint"), ("sparkle", "glint")]


def texture(image):
    name = os.path.basename(str((image or {}).get("filename", ""))).lower()
    kind = next((k for word, k in TEXTURES if word in name), "dirt")
    seed = sum(ord(c) * (i + 1) for i, c in enumerate(name)) % 997
    return {"generate": kind, "seed": seed}


# The settings MillSaph's styles and lenses read (the names in their sources), and "name": a
# style's or lens's other settings, which MillSaph has no use for, are left out.
STYLE_SOURCES = ["glare_style.c", "lensflare_style.c", "glare_build.h", "ms_lensflare.c",
                 "ms_glare.c"]


def style_keys():
    keys = {"name"}
    for f in STYLE_SOURCES:
        with open(os.path.join(ROOT, "src", "effects", f)) as src:
            keys |= set(re.findall(r'"([a-z_][a-z0-9_]*)"', src.read()))
    return keys


def prune(v, keys):
    if isinstance(v, dict):
        return {k: prune(x, keys) for k, x in v.items() if k in keys}
    if isinstance(v, list):
        return [prune(x, keys) for x in v]
    return v


def own_textures(style):
    images = style.get("images") or []

    def walk(elements):
        for e in elements:
            image = e.get("image")
            if isinstance(image, dict) and "generate" not in image:
                k = image.get("save_index")
                e["image"] = texture(images[k] if isinstance(k, int) and 0 <= k < len(images)
                                     else image)
            walk(e.get("children", []))

    walk(style.get("elements", []))
    style.pop("images", None)
    style.pop("thumbnail", None)
    return prune(style, KEYS)


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "preset"


def convert(effect):
    with open(os.path.join(HOME, "presets", effect + ".json"), encoding="utf-8") as f:
        presets = json.load(f)["presets"]
    with open(os.path.join(HOME, "describe", effect + ".json")) as f:
        d = json.load(f)
    ref = {k["name"]: k for k in d["reference"]["knobs"]}
    ms = {k["name"]: k for k in d["millsaph"]["knobs"]}
    missing, notes = {}, set()
    out_dir = os.path.join(ROOT, "presets", effect.lower())
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):  # this script's earlier files only (MillSaph's own stay)
        path = os.path.join(out_dir, old)
        if old.endswith(".json"):
            with open(path, encoding="utf-8") as f:
                if json.load(f).get("converted"):
                    os.remove(path)
    for name, p in sorted(presets.items()):
        params = {}
        for k, v in p["params"].items():
            if "mocha" in k or k in DROP:
                continue
            k = RENAME.get(effect, {}).get(k, k)
            if k in SPLIT and isinstance(v, list):
                for kk, vv in zip(SPLIT[k], v):
                    params[kk] = vv
                continue
            if isinstance(v, dict):  # a style or a lens
                v = json.dumps(own_textures(v), separators=(",", ":"), ensure_ascii=False)
            elif k in POINTS and isinstance(v, list):
                v = [0.5 + v[0], 0.5 + v[1]]  # (only [0, 0] in the files: the centre)
            elif k in WIDTHS and isinstance(v, (int, float)):
                v = v * WIDTH_SCALE.get(k, 1 / 8)
                if (effect, k) in WIDTH_MIN:
                    v = max(v, WIDTH_MIN[(effect, k)])
            elif k in ref and "choices" in ref[k] and isinstance(v, (int, float)):
                entries = ref[k]["choices"]
                label = entries[int(v)] if 0 <= int(v) < len(entries) else None
                ours = [c.lower() for c in ms.get(k, {}).get("choices", [])]
                if label and label.lower() in ours:
                    v = ours.index(label.lower())
                elif ours:
                    notes.add("%s: %s not in %s, last entry used" % (k, label, ours))
                    v = len(ours) - 1
            params[k] = v
            if k not in ms:
                missing.setdefault(k, []).append(name)
        # (Glare's Default: the reference's own default style, Starstrike, not MillSaph's)
        if effect == "Glare" and name == "Default" and "Starstrike" in presets:
            params.setdefault("style_", json.dumps(
                own_textures(dict(presets["Starstrike"]["params"]["type"])),
                separators=(",", ":"), ensure_ascii=False))
        out = {"name": name, "tags": p.get("tags_v8", []), "converted": True, "params": params}
        path = os.path.join(out_dir, slug(name) + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1, ensure_ascii=False)
            f.write("\n")
    print("%-18s %3d presets -> presets/%s" % (effect, len(presets), effect.lower()))
    for k, users in sorted(missing.items()):
        print("    not in MillSaph yet: %-22s (%d presets)" % (k, len(users)))
    for n in sorted(notes):
        print("    " + n)


KEYS = style_keys()

if __name__ == "__main__":
    for e in sys.argv[1:] or EFFECTS:
        convert(e)
