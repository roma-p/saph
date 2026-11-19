#!/usr/bin/env python3
# The reference's presets, read from its preset files, for MillSaph's equivalent presets.
#
#   python3 tools/measure/reference_presets.py <presets folder> [Effect ...]
#
# -> ~/millsaph_measure/presets/<Effect>.json, one per effect (all of them by default):
#   {"effect": "Glow", "presets": {"<name>": {
#       "file": file name, "tags": {group: [tag, ...]}, "tags_v8": [tag, ...],
#       "description": text,
#       "inputs": {input: [width, height]},   the image size the preset was made at (widths)
#       "params": {param: value}}}}           a number, a list (a colour), or a style (dict)
# Only the presets the reference's preset window lists: those not marked deprecated. Besides
# the preset files: the styles of the glares/ folder next to the presets folder (Glare
# presets, their style as the "type" setting, as in the preset files) and the lenses of the
# lensflares/ folder (Lens Flare presets, their lens as the "lens" setting).
# The files hold a metadata segment then a settings segment, in JSON, with one header line
# each; a deprecated one's settings are not read. Settings without a value (placeholders,
# empty tracking data) are left out. The descriptions are for working out the presets;
# MillSaph's presets get their own.
import json, os, re, sys

EFFECTS = ["BandPass", "EdgeAwareBlur", "Distort", "EdgeBlur", "RackDefocus", "Glow", "Glint",
           "Glare", "LensFlare", "Flicker", "FlickerMatch", "FlickerMatchColor", "TVDamage",
           "JpegDamage", "PixelSort", "Mosaic"]
OUT = os.path.expanduser("~/millsaph_measure/presets")
SEPARATOR = "# segment separator do not remove this line"
SKIPPED_TYPES = (None, "mocha_data")
# File name escapes: "_" and the character's hex code, four digits for typographic quotes and
# dashes ("_201c"), two for a punctuation or a space ("_20"); other underscores are the name's.
TYPOGRAPHIC = (0x2013, 0x2014) + tuple(range(0x2018, 0x2020))
PUNCTUATION = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"


def decode(stem):
    def char(m):
        code = m.group(1)
        if len(code) == 4 and int(code, 16) in TYPOGRAPHIC:
            return chr(int(code, 16))
        c = chr(int(code[:2], 16))
        return c + code[2:] if c in PUNCTUATION else m.group(0)
    return re.sub(r"_([0-9a-fA-F]{4}|[0-9a-fA-F]{2})", char, stem)


def parse(segment):
    # a header line ("# 1 ..." on the first segment), then JSON
    return json.loads(segment[segment.index("{"):])


def read_file(path):
    # (metadata, settings), settings None for a deprecated file (not read)
    with open(path, encoding="utf-8") as f:
        parts = f.read().split(SEPARATOR)
    info = parse(parts[0]).get("metadata", {})
    if info.get("deprecated"):
        return info, None
    return info, parse(parts[1])


def entry(f, info, params, inputs=None):
    return {"file": f, "tags": info.get("tags", {}), "tags_v8": info.get("tags-v8", []),
            "description": info.get("description", ""), "inputs": inputs or {},
            "params": params}


def value(param):
    if param.get("type") == "flare":
        return param["value"]
    channels = [c["value"] for c in param.get("channels", [])]
    return channels[0] if len(channels) == 1 else channels


def read(folder, effect):
    prefix = "S_%s_" % effect
    presets = {}
    for f in sorted(os.listdir(folder)):
        stem, ext = os.path.splitext(f)
        if ext != ".gp" or not stem.startswith(prefix):
            continue
        info, settings = read_file(os.path.join(folder, f))
        if settings is None:
            continue
        name = (info.get("preset-name") or decode(stem[len(prefix):])).strip()
        presets[name] = entry(
            f, info,
            {k: value(v) for k, v in sorted(settings.get("params", {}).items())
             if v.get("type") not in SKIPPED_TYPES},
            {k: [v.get("xdim"), v.get("ydim")] for k, v in settings.get("inputs", {}).items()})
    # styles and lenses: their own folders
    extra = {"Glare": ("glares", ".glare", "type"), "LensFlare": ("lensflares", ".flare", "lens")}
    if effect in extra:
        sub, ext, param = extra[effect]
        styles = os.path.join(os.path.dirname(os.path.abspath(folder)), sub)
        for f in sorted(os.listdir(styles)) if os.path.isdir(styles) else []:
            if not f.endswith(ext):
                continue
            info, style = read_file(os.path.join(styles, f))
            if style is None:
                continue
            name = (info.get("preset-name") or decode(f[:-len(ext)])).strip()
            presets[name] = entry(f, info, {param: style})
    return presets


def main(args):
    if not args or not os.path.isdir(args[0]):
        sys.exit("usage: reference_presets.py <presets folder> [Effect ...]")
    os.makedirs(OUT, exist_ok=True)
    for effect in args[1:] or EFFECTS:
        presets = read(args[0], effect)
        path = os.path.join(OUT, effect + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"effect": effect, "presets": presets}, f, indent=1, ensure_ascii=False)
        print("%-18s %3d presets -> %s" % (effect, len(presets), path))


if __name__ == "__main__":
    main(sys.argv[1:])
