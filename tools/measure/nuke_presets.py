# The reference's presets as Nuke shows them once loaded: what its preset files' settings
# become as knobs (units, renamed settings), to check reference_presets.py's reading. The
# reference loads a preset through its own window only (its Load Preset button), so this takes
# two steps with a person in between:
#
#   create  one reference node per preset of a list (default:
#           ~/millsaph_measure/presets/calibration.json, {"Effect": ["preset name", ...]}),
#           labelled with the preset's name, one row per effect, plus one node at the defaults
#           per effect
#   (then, in Nuke's interface: on each labelled node, Load Preset, pick the preset its label
#   names; save the script)
#   dump    every node's knobs (as Nuke writes them), the presets' and the defaults', into
#           ~/millsaph_measure/presets/nuke_knobs.json, merged with what is already there;
#           lists the nodes still at the defaults (preset not loaded?)
#
# The project format is set to 1920x1080 first (widths are fractions of the project width).
#
# Script Editor:  MILLSAPH_PRESETS = "create"    (or "create <list.json>", or "dump")
#                 exec(open("<MillSaph>/tools/measure/nuke_presets.py").read())
# Headless dump:  nuke -t tools/measure/nuke_presets.py dump <saved script.nk>
import json, os, sys, nuke

REFERENCE = "OFXcom.genarts.sapphire."
# Effect: the reference's node class (as in nuke_measure.py's EFFECTS).
EFFECTS = {
    "BandPass": "Stylize.S_BandPass_v1",
    "EdgeAwareBlur": "BlurSharpen.S_EdgeAwareBlur_v1",
    "Distort": "Distort.S_Distort_v1",
    "EdgeBlur": "BlurSharpen.S_EdgeBlur_v1",
    "RackDefocus": "BlurSharpen.S_RackDefocus_v1",
    "Glow": "Lighting.S_Glow_v1",
    "Glint": "Lighting.S_Glint_v1",
    "Glare": "Lighting.S_Glare_v1",
    "LensFlare": "Lighting.S_LensFlare_v1",
    "Flicker": "Time.S_Flicker_v1",
    "FlickerMatch": "Time.S_FlickerMatch_v1",
    "FlickerMatchColor": "Time.S_FlickerMatchColor_v1",
    "TVDamage": "Stylize.S_TVDamage_v1",
    "JpegDamage": "Stylize.S_JpegDamage_v1",
    "PixelSort": "Stylize.S_PixelSort_v1",
    "Mosaic": "Stylize.S_Mosaic_v1",
}
HOME = os.path.expanduser("~/millsaph_measure/presets")
FORMAT = "1920 1080 0 0 1920 1080 1 millsaph_presets"
# Knobs of every node, not of the effect.
NODE_KNOBS = {"name", "xpos", "ypos", "label", "selected", "note_font", "note_font_size",
              "note_font_color", "tile_color", "gl_color", "hide_input", "cached",
              "postage_stamp", "postage_stamp_frame", "disable", "dope_sheet", "bookmark",
              "lifetimeStart", "lifetimeEnd", "useLifetime", "icon", "indicators",
              "process_mask", "help", "onCreate", "onDestroy", "knobChanged", "updateUI",
              "autolabel", "panel", "cacheLocal", "inputChange", "rootNodeUpdated"}
TAG_EFFECT, TAG_PRESET = "millsaph_effect", "millsaph_preset"
ROW, COLUMN = 160, 120  # layout, in Nuke's units


def node(effect, x, y, preset=""):
    cls = REFERENCE + EFFECTS[effect]
    try:
        n = nuke.createNode(cls, inpanel=False)
    except RuntimeError:  # Linux Nuke can list OFX classes in lower case
        n = nuke.createNode("OFX" + cls[3:].lower(), inpanel=False)
    n.setInput(0, None)
    n.setXYpos(x, y)
    for tag, value in ((TAG_EFFECT, effect), (TAG_PRESET, preset)):
        k = nuke.String_Knob(tag, tag)
        k.setFlag(nuke.INVISIBLE)
        n.addKnob(k)
        k.setValue(value)
    n["label"].setValue(preset or "DEFAULTS (leave as is)")
    if not preset:
        n["tile_color"].setValue(0x555555FF)
    return n


def create(path):
    nuke.addFormat(FORMAT)
    nuke.root()["format"].setValue("millsaph_presets")
    with open(os.path.expanduser(path)) as f:
        lists = json.load(f)
    y, total = 0, 0
    for effect, presets in lists.items():
        node(effect, 0, y)
        for i, name in enumerate(presets):
            node(effect, (i + 1) * COLUMN, y, name)
        b = nuke.nodes.BackdropNode(xpos=-20, ypos=y - 60,
                                    bdwidth=(len(presets) + 1) * COLUMN + 20, bdheight=ROW - 20,
                                    label=effect, note_font_size=30)
        b["tile_color"].setValue(0x3A3A4AFF)
        total += len(presets)
        y += ROW
    print("%d preset nodes. On each: Load Preset, pick the preset its label names; save the "
          "script (e.g. %s/presets.nk), then dump." % (total, HOME))


def knob_values(n):
    out = {}
    for name, k in n.knobs().items():
        if name in NODE_KNOBS or name.startswith("millsaph_") or k.Class() in (
                "Script_Knob", "PyScript_Knob", "Tab_Knob", "Text_Knob", "Obsolete_Knob"):
            continue
        try:
            out[name] = k.toScript()
        except Exception:
            pass
    return out


def tag(n, name):
    k = n.knob(name)
    return k.value() if k is not None else None


def dump():
    path = os.path.join(HOME, "nuke_knobs.json")
    result = {}
    if os.path.exists(path):
        with open(path) as f:
            result = json.load(f)
    nodes = [n for n in nuke.allNodes() if tag(n, TAG_EFFECT)]
    for n in nodes:
        r = result.setdefault(tag(n, TAG_EFFECT), {"defaults": {}, "presets": {}})
        if not tag(n, TAG_PRESET):
            r["defaults"] = knob_values(n)
    pending = []
    for n in nodes:
        effect, preset = tag(n, TAG_EFFECT), tag(n, TAG_PRESET)
        if not preset:
            continue
        values = knob_values(n)
        if values == result[effect]["defaults"]:
            pending.append("%s / %s" % (effect, preset))
            continue
        result[effect]["presets"][preset] = values
    result["_format"] = nuke.root().format().width()
    with open(path, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    print("%d presets -> %s" % (sum(len(r["presets"]) for k, r in result.items()
                                    if not k.startswith("_")), path))
    if pending:
        print("%d nodes still at the defaults (preset not loaded?):" % len(pending))
        for p in pending:
            print("  " + p)


def main(args):
    if args and args[0] == "create":
        create(args[1] if len(args) > 1 else os.path.join(HOME, "calibration.json"))
    elif args and args[0] == "dump":
        if len(args) > 1:
            nuke.scriptOpen(os.path.expanduser(args[1]))
        dump()
    else:
        print("usage: nuke_presets.py create [list.json] | dump [script.nk]")


if "MILLSAPH_PRESETS" in globals():  # Script Editor
    main(MILLSAPH_PRESETS.split())
else:  # nuke -t
    main(sys.argv[1:])
