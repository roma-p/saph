# Measurements in Nuke, for recreating an effect (tools/measure/README.md). Two jobs:
#
#   describe  writes every knob of the reference node (and of MillSaph's, when it exists) of
#             the named effects: name, label, class, value at creation (the default), slider
#             range, choices, tooltip, flags; and the number of inputs.
#             -> ~/millsaph_measure/describe/<Effect>.json and .txt
#   jobs      renders a jobs file (JSON, below) with the reference node and MillSaph's.
#             -> <out>/reference/<job>.exr and <out>/millsaph/<job>.exr, to compare with
#                tools/measure/compare.py <out>/reference <out>/millsaph
#
# Headless:      nuke -t tools/measure/nuke_measure.py describe Glow Glint     (or "all")
#                nuke -t tools/measure/nuke_measure.py jobs ~/millsaph_measure/jobs/glow_1.json
# Script Editor (the interface, where the reference plugin may only be licensed there):
#                MILLSAPH_MEASURE = "jobs ~/millsaph_measure/jobs/glow_1.json"
#                exec(open("<MillSaph>/tools/measure/nuke_measure.py").read())
#
# The reference plugin renders a watermark (black with a red cross) when it is not licensed;
# every jobs run first renders its BandPass and MillSaph's on a noise image and says whether
# they match (they do to about 1e-5 when it is licensed).
#
# Jobs file:
#   {
#     "size": [256, 128],                       project format: the normalised widths of both
#                                               plugins are fractions of its width
#     "probes": "~/millsaph_measure/probes",    where the input names below are (EXR)
#     "out": "~/millsaph_measure/renders/glow_1",
#     "vendors": ["reference", "millsaph"],     optional, these two by default
#     "jobs": [
#       {"name": "impulse_default", "effect": "Glow", "inputs": ["impulse"]},
#       {"name": "impulse_width4", "effect": "Glow", "inputs": ["impulse", null, "flat"],
#        "knobs": {"width": 4, "color": [1, 0.5, 0.2], "mode": "Screen"}},
#       {"name": "flicker", "effect": "Flicker", "inputs": ["seq/noise.####"],
#        "frames": [1, 10], "knobs": {"seed": 3}}
#     ]
#   }
# inputs: probe names (without .exr) by input index, null to leave one unconnected; a name
# with #### is a sequence over "frames" (default [1, 1]); with more than one frame the outputs
# are <job>.####.exr.
# knobs: numbers, lists (per component), true / false, choice names or indices. Knobs are
# named as `describe` lists them; a knob one node does not have is reported and skipped (the
# job still renders). A job's "vendors" overrides the file's; a job with "node": "<Nuke class>"
# renders that node instead (e.g. "Blur", to check a hypothesis against a Nuke node).
import json, os, sys, time, nuke

REFERENCE = "OFXcom.genarts.sapphire."
MILLSAPH = "OFXcom.xiaoluo.millsaph."
# Effect: reference node class, MillSaph node class (None: not written yet).
EFFECTS = {
    "BandPass": ("Stylize.S_BandPass_v1", "BandPass_v1"),
    "EdgeAwareBlur": ("BlurSharpen.S_EdgeAwareBlur_v1", "EdgeAwareBlur_v1"),
    "Distort": ("Distort.S_Distort_v1", "Distort_v1"),
    "EdgeBlur": ("BlurSharpen.S_EdgeBlur_v1", None),
    "RackDefocus": ("BlurSharpen.S_RackDefocus_v1", None),
    "Glow": ("Lighting.S_Glow_v1", None),
    "Glint": ("Lighting.S_Glint_v1", None),
    "Glare": ("Lighting.S_Glare_v1", None),
    "LensFlare": ("Lighting.S_LensFlare_v1", None),
    "Flicker": ("Time.S_Flicker_v1", None),
    "FlickerMatch": ("Time.S_FlickerMatch_v1", None),
    "FlickerMatchColor": ("Time.S_FlickerMatchColor_v1", None),
    "TVDamage": ("Stylize.S_TVDamage_v1", None),
    "JpegDamage": ("Stylize.S_JpegDamage_v1", None),
    "PixelSort": ("Stylize.S_PixelSort_v1", None),
    "Mosaic": ("Stylize.S_Mosaic_v1", None),
}
HOME = os.path.expanduser("~/millsaph_measure")
created = []


def node_class(effect, vendor):
    ref, ms = EFFECTS[effect]
    if vendor == "reference":
        return REFERENCE + ref
    return MILLSAPH + ms if ms else None


def create(cls):
    try:
        n = nuke.createNode(cls, inpanel=False)
    except RuntimeError:  # Linux Nuke can list OFX classes in lower case
        n = nuke.createNode("OFX" + cls[3:].lower(), inpanel=False)
    created.append(n)
    return n


def drop(n):
    try:
        nuke.delete(n)
    except Exception:
        pass
    if n in created:
        created.remove(n)


def value_of(k):
    try:
        v = k.value()
    except Exception:
        return None
    try:
        json.dumps(v)
        return v
    except TypeError:
        return str(v)


# Every knob of a node, as a list of dicts.
def knobs(n):
    out = []
    for k in n.allKnobs():
        d = {"name": k.name(), "label": k.label(), "class": k.Class(), "value": value_of(k)}
        for attr in ("min", "max"):
            try:
                d[attr] = getattr(k, attr)()
            except Exception:
                pass
        try:
            d["choices"] = list(k.values())
        except Exception:
            pass
        try:
            d["tooltip"] = k.tooltip()
        except Exception:
            pass
        try:
            d["hidden"] = bool(k.getFlag(nuke.INVISIBLE))
        except Exception:
            pass
        out.append(d)
    return out


def describe(names):
    folder = os.path.join(HOME, "describe")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    names = list(EFFECTS) if names in ([], ["all"]) else names
    for effect in names:
        result = {"effect": effect}
        for vendor in ("reference", "millsaph"):
            cls = node_class(effect, vendor)
            if not cls:
                continue
            try:
                n = create(cls)
            except Exception as ex:
                result[vendor] = {"class": cls, "error": str(ex)}
                continue
            labels = []
            for i in range(n.maxInputs()):
                try:
                    labels.append(n.inputLabel(i))
                except Exception:
                    labels.append(str(i))
            result[vendor] = {"class": cls, "inputs": labels, "knobs": knobs(n)}
            drop(n)
        with open(os.path.join(folder, effect + ".json"), "w") as f:
            json.dump(result, f, indent=1)
        with open(os.path.join(folder, effect + ".txt"), "w") as f:
            for vendor in ("reference", "millsaph"):
                r = result.get(vendor)
                if not r:
                    continue
                f.write("== %s %s\n" % (vendor, r["class"]))
                if "error" in r:
                    f.write("  error: %s\n" % r["error"])
                    continue
                f.write("  inputs: %s\n" % ", ".join(r["inputs"]))
                for d in r["knobs"]:
                    extra = ""
                    if "min" in d:
                        extra += " range %s..%s" % (d["min"], d["max"])
                    if "choices" in d:
                        extra += " choices %s" % d["choices"]
                    if d.get("hidden"):
                        extra += " hidden"
                    f.write("  %-28s %-22s %-18s = %s%s\n" % (
                        d["name"], d["label"][:22], d["class"], d["value"], extra))
                    if d.get("tooltip"):
                        f.write("      %s\n" % d["tooltip"].replace("\n", " ")[:300])
        print("%s: %s" % (effect, os.path.join(folder, effect + ".txt")))


def set_knob(n, name, value):
    k = n.knob(name)
    if k is None:
        return False
    if k.Class() == "Enumeration_Knob" and isinstance(value, str):
        k.setValue(value)
    elif isinstance(value, bool):
        k.setValue(value)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            k.setValue(v, i)
    else:
        k.setValue(value)
    return True


def reader(path, frames):
    r = nuke.nodes.Read()
    r["file"].fromUserText(path)
    if "#" in path:
        r["first"].setValue(frames[0])
        r["last"].setValue(frames[1])
    r["raw"].setValue(True)
    created.append(r)
    return r


def render(n, path, frames):
    w = nuke.nodes.Write(inputs=[n], file=path, file_type="exr", datatype="32 bit float",
                         compression="none", channels="rgba")
    w["raw"].setValue(True)
    created.append(w)
    nuke.execute(w, frames[0], frames[1])
    drop(w)


def licence_check():
    import random
    rng = random.Random(1)
    if not os.path.isdir(HOME):
        os.makedirs(HOME)
    path = os.path.join(HOME, "licence_noise.exr")
    noise = nuke.nodes.Noise(size=8)
    created.append(noise)
    render(noise, path, [1, 1])
    drop(noise)
    src = reader(path, [1, 1])
    try:
        a, b = create(node_class("BandPass", "reference")), create(node_class("BandPass", "millsaph"))
    except Exception as ex:
        return "not checked (%s)" % ex
    a.setInput(0, src)
    b.setInput(0, src)
    m = nuke.nodes.Merge2(inputs=[a, b], operation="difference")
    created.append(m)
    w, h = src.width(), src.height()
    worst = 0.0
    for _ in range(64):
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        for c in ("rgba.red", "rgba.green", "rgba.blue"):
            worst = max(worst, abs(m.sample(c, x, y)))
    for n in (m, a, b, src):
        drop(n)
    if worst < 0.01:
        return "OK (its BandPass matches MillSaph's: largest difference %.2g)" % worst
    return ("NOT LICENSED? its BandPass differs from MillSaph's by %.2g: it probably renders its "
            "watermark; its renders are not usable" % worst)


def jobs(path):
    path = os.path.expanduser(path)
    spec = json.load(open(path))
    w, h = spec["size"]
    probes = os.path.expanduser(spec.get("probes", os.path.join(HOME, "probes")))
    out = os.path.expanduser(spec["out"])
    name = "MS_%dx%d" % (w, h)
    if not [f for f in nuke.formats() if f.name() == name]:
        nuke.addFormat("%d %d 1 %s" % (w, h, name))
    nuke.root()["format"].setValue(name)
    if not os.path.isdir(out):
        os.makedirs(out)
    log = open(os.path.join(out, "jobs.log"), "w")

    def say(*a):
        s = " ".join(str(x) for x in a)
        print(s)
        log.write(s + "\n")
        log.flush()

    say("Nuke %s, %s: %d jobs at %d x %d" % (nuke.NUKE_VERSION_STRING, path, len(spec["jobs"]), w, h))
    say("Reference licence: " + licence_check())
    start = time.time()
    for job in spec["jobs"]:
        frames = job.get("frames", [1, 1])
        vendors = job.get("vendors", spec.get("vendors", ["reference", "millsaph"]))
        if "node" in job:
            vendors = ["nuke"]
        for vendor in vendors:
            cls = job["node"] if vendor == "nuke" else node_class(job["effect"], vendor)
            if not cls:
                continue
            folder = os.path.join(out, vendor)
            if not os.path.isdir(folder):
                os.makedirs(folder)
            try:
                n = create(cls)
                inputs = []
                for i, image in enumerate(job.get("inputs", [])):
                    if image is None:
                        continue
                    r = reader(os.path.join(probes, image + ".exr"), frames)
                    inputs.append(r)
                    n.setInput(i, r)
                missing = [k for k, v in job.get("knobs", {}).items() if not set_knob(n, k, v)]
                seq = ".####" if frames[0] != frames[1] else ""
                render(n, os.path.join(folder, job["name"] + seq + ".exr"), frames)
                say("%-10s %-32s ok%s" % (vendor, job["name"],
                                          "  (no knob %s)" % ", ".join(missing) if missing else ""))
            except Exception as ex:
                say("%-10s %-32s FAILED: %s" % (vendor, job["name"], ex))
            for x in list(created):
                drop(x)
    say("done in %.0f s; compare: python3 tools/measure/compare.py %s/reference %s/millsaph" % (
        time.time() - start, out, out))
    log.close()


def main(args):
    try:
        if args and args[0] == "describe":
            describe(args[1:])
        elif len(args) == 2 and args[0] == "jobs":
            jobs(args[1])
        else:
            print("usage: nuke_measure.py describe <Effect ...|all> | jobs <file.json>")
    finally:
        for n in list(created):
            drop(n)


if "MILLSAPH_MEASURE" in globals():  # Script Editor
    main(MILLSAPH_MEASURE.split())
else:  # nuke -t
    main(sys.argv[1:])
