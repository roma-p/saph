# Times, in Nuke, every setting of `millsaph_bench --sweep` (its millsaph_sweep.csv: each
# parameter of each effect over a series of values) on MillSaph's node and on its Sapphire
# counterpart, at 4K, after the sweep. Either in the Script Editor of Nuke:
#
#   MILLSAPH_SWEEP_CSV = "~/millsaph_perf/millsaph_sweep.csv"   # optional: where it is
#   MILLSAPH_SWEEP_EFFECT = "MS_EdgeAwareBlur"                      # optional: only this one
#   MILLSAPH_SWEEP_MATCH = "input="          # optional: only the settings with this in them
#   exec(open("<MillSaph>/tools/perf/nuke_sweep.py").read())
#
# (tools/perf/sweep_all.sh says what to run), or `nuke -t nuke_sweep.py` with the same settings
# as environment variables. Sapphire needs its licence (Nuke started without its interface may
# not find it), else it does not render its effect: the script first compares Sapphire's
# BandPass with MillSaph's (the same at their defaults) and says whether Sapphire looks
# licensed.
#
# Times are Nuke's performance timers (the time spent in the node: best render after the first),
# as nuke_time.py, and MillSaph's own time for the effect, from its log (~/millsaph.log). WIDTH and
# HEIGHT values are set in pixels of the 4K frame, as Nuke shows them. A setting a node does not
# have is reported, not timed.
# Results: printed, and written to <dir>/sweep_nuke.csv and sweep_nuke.txt, followed by
# where MillSaph ran (its log meanwhile, without each render's lines).
import csv, os, re, time, nuke

def setting(name, default):  # Script Editor variable, else environment variable
    return globals().get(name, os.environ.get(name, default))

HERE = os.path.expanduser(setting("MILLSAPH_PERF_DIR", "~/millsaph_perf"))
CSV = os.path.expanduser(setting("MILLSAPH_SWEEP_CSV",
                                 os.path.join(HERE, "millsaph_sweep.csv")))
ONLY = setting("MILLSAPH_SWEEP_EFFECT", "")
MATCH = setting("MILLSAPH_SWEEP_MATCH", "")  # only the settings with this in them
REPS = 4
WIDTH, HEIGHT = 3840, 2160
SAPPHIRE = "OFXcom.genarts.sapphire."
MILLSAPH = "OFXcom.xiaoluo.millsaph."
# MillSaph effect: Sapphire node, MillSaph node
NODES = {
    "MS_BandPass": (SAPPHIRE + "Stylize.S_BandPass_v1", MILLSAPH + "BandPass_v1"),
    "MS_EdgeAwareBlur": (SAPPHIRE + "BlurSharpen.S_EdgeAwareBlur_v1",
                         MILLSAPH + "EdgeAwareBlur_v1"),
    "MS_Distort": (SAPPHIRE + "Distort.S_Distort_v1", MILLSAPH + "Distort_v1"),
}
PLUGIN_LOG = os.path.expanduser("~/millsaph.log")  # src/common/log.h
# The test image, as nuke_time.py: gradients, a disc and a box with hard edges, a little noise.
DISC = "pow(x/width-0.39,2)*256/81+pow(y/height-0.46,2)<0.077"
BOX = "x>width*0.68&&x<width*0.88&&y>height/7&&y<height*5/6"
NOISE = "(random(x,y,%d)-0.5)*0.04"

if not os.path.isdir(HERE):
    os.makedirs(HERE)
text = open(os.path.join(HERE, "sweep_nuke.txt"), "w")
out = open(os.path.join(HERE, "sweep_nuke.csv"), "w")
out.write("effect,mode,parameter,value,Sapphire ms,MillSaph ms,MillSaph x,"
          "MillSaph effect ms,note\n")
log_start = os.path.getsize(PLUGIN_LOG) if os.path.exists(PLUGIN_LOG) else 0
def say(*a):
    s = " ".join(str(x) for x in a); print(s); text.write(s + "\n"); text.flush()

if not [f for f in nuke.formats() if f.name() == "MS_UHD"]:
    nuke.addFormat("%d %d 1 MS_UHD" % (WIDTH, HEIGHT))
nuke.root()["format"].setValue("MS_UHD")
created = []

def create(cls):
    try: return nuke.createNode(cls, inpanel=False)
    except RuntimeError: return nuke.createNode("OFX" + cls[3:].lower(), inpanel=False)  # Linux

def write(node, path):
    w = nuke.nodes.Write(inputs=[node], file=path, file_type="exr", datatype="32 bit float",
                         compression="none", channels="rgba")
    w["raw"].setValue(True)
    created.append(w)
    return w

def squash(name):
    return name.replace("_", "").replace(" ", "").lower()

# The test images millsaph_bench --sweep writes (tools/perf/images.c), each read once, behind
# a Grade (see timed()).
IMAGES = os.path.join(HERE, "images")
pictures = {}
def picture(name):
    if name not in pictures:
        r = nuke.nodes.Read(file=os.path.join(IMAGES, name + ".exr")); r["raw"].setValue(True)
        g = nuke.nodes.Grade(inputs=[r])
        created.extend([r, g])
        pictures[name] = g
    return pictures[name]

# Applies "name=value ..." (CHOICE as an index; a fraction of the frame width / height followed
# by w / h) to `n`; connects each "input=Clip/index" to `image`, "input=Clip/index/name" to that
# test image ("image=name": the Source's, see below). Returns what `n` does not have.
def apply(n, setting, image):
    missing = []
    for item in setting.split():
        name, value = item.split("=", 1)
        if name == "image":
            continue
        if name == "input":  # Clip/index: by its label where Nuke gives it, else its place
            parts = value.split("/")
            clip, index = parts[0], parts[1]
            target = picture(parts[2]) if len(parts) > 2 else image
            label = getattr(n, "inputLabel", None)
            found = [i for i in range(n.maxInputs()) if label and squash(label(i)) == squash(clip)]
            i = found[0] if found else int(index)
            if i < n.maxInputs():
                n.setInput(i, target)
            else:
                missing.append("input " + clip)
            continue
        k = n.knob(name)
        if k is None:
            missing.append(name)
            continue
        scale = {"w": WIDTH, "h": HEIGHT}.get(value[-1], 1)
        v = float(value.rstrip("wh")) * scale
        if k.Class() in ("Enumeration_Knob",):
            k.setValue(int(v))
        elif k.Class() in ("Boolean_Knob",):
            k.setValue(bool(int(v)))
        else:
            k.setValue(v)
    return missing

# Node time per render (performance timers), best after the first; the Grade above changes
# by 1e-6 each time so that Nuke cannot reuse a cached result. Nuke's timers sometimes give 0 for
# a render: those renders are left out. Where they give nothing for any render, the whole frame's
# time minus `base` (reading + writing a frame without effect) instead. Returns (ms, how):
# how is "" for the timers, "frame" otherwise. With `log`, also MillSaph's own effect time from
# its log (the "effect" of each render's line), best after the first: (ms, how, own ms).
base = 0
def timed(n, grade, frames_only=False, log=False):
    w = write(n, os.path.join(HERE, "out.exr"))
    nodes, frames, own = [], [], []
    for rep in range(REPS):
        grade["white"].setValue(1 + rep * 1e-6)
        try: nuke.clearRAMCache()
        except Exception: pass
        nuke.resetPerformanceTimers()
        start = os.path.getsize(PLUGIN_LOG) if os.path.exists(PLUGIN_LOG) else 0
        t = time.time()
        nuke.execute(w, 1, 1)
        frame = (time.time() - t) * 1000
        try:
            node = float(n.performanceInfo(nuke.PROFILE_ENGINE)["timeTakenWall"])
        except Exception:
            node = 0
        if rep:
            frames.append(frame)
            if node > 0: nodes.append(node)
        if log and rep and os.path.exists(PLUGIN_LOG):
            with open(PLUGIN_LOG) as f:
                f.seek(start)
                own += [float(m) for m in re.findall(r" effect ([0-9.]+),", f.read())]
    nuke.delete(w); created.remove(w)
    if frames_only:
        ms, how = min(frames), ""
    elif nodes:
        ms, how = min(nodes), ""
    else:
        ms, how = max(min(frames) - base, 1), "frame"
    return (ms, how, min(own) if own else None) if log else (ms, how)

# Largest difference between two nodes' images, sampled on a grid (RGB).
def difference(a, b):
    m = nuke.nodes.Merge2(inputs=[a, b], operation="difference"); created.append(m)
    worst = 0.0
    for gx in range(16):
        for gy in range(9):
            x, y = (gx + 0.5) * WIDTH / 16, (gy + 0.5) * HEIGHT / 9
            for c in ("rgba.red", "rgba.green", "rgba.blue"):
                worst = max(worst, abs(m.sample(c, x, y)))
    nuke.delete(m); created.remove(m)
    return worst

# Sapphire draws over its image without a licence: its BandPass, at the defaults, is MillSaph's
# (to about 1e-5).
def licence_check(image):
    try:
        a, b = create(NODES["MS_BandPass"][0]), create(NODES["MS_BandPass"][1])
    except Exception as ex:
        return "not checked (%s)" % ex
    created.extend([a, b])
    a.setInput(0, image); b.setInput(0, image)
    d = difference(a, b)
    for n in (a, b):
        nuke.delete(n); created.remove(n)
    if d < 0.01:
        return "OK (Sapphire's BandPass matches MillSaph's: largest difference %.2g)" % d
    return ("WARNING: Sapphire's BandPass differs from MillSaph's by %.2g: Sapphire is probably "
            "not licensed here (it draws over its image), its times are not comparable" % d)

# Why MillSaph's nodes are missing: where Nuke looks for OFX plugins, the MillSaph bundles there
# and whether they load (the reason, when not), and the OFX nodes Nuke knows of.
def diagnose():
    import ctypes, glob
    say("MillSaph's nodes are missing in this Nuke. OFX_PLUGIN_PATH = %s" %
        os.environ.get("OFX_PLUGIN_PATH"))
    dirs = [d for d in os.environ.get("OFX_PLUGIN_PATH", "").split(os.pathsep) if d]
    for d in dirs + ["/usr/OFX/Plugins", "/Library/OFX/Plugins"]:
        for f in glob.glob(os.path.join(d, "MillSaph.ofx.bundle", "Contents", "*", "MillSaph.ofx")):
            try:
                ctypes.CDLL(f)
                say("  %s: loads" % f)
            except OSError as ex:
                say("  %s: does not load: %s" % (f, ex))
    known = [p for p in nuke.plugins(nuke.ALL | nuke.NODIR) if "millsaph" in p.lower()]
    say("  Nuke's MillSaph entries: %s" % (known or "none"))
    say("  Nuke's OFX cache (stale entries can hide a plugin): %s" %
        glob.glob(os.path.expanduser("~/.nuke/ofxplugincache/*")))

# Per effect and mode: how many settings MillSaph is faster on, the range of the ratio, and the
# settings it is slowest on.
def summary(results):
    say("\nSummary (x: how many times faster MillSaph is; under 1: slower)")
    keys = []
    for r in results:
        if (r[0], r[1]) not in keys: keys.append((r[0], r[1]))
    for effect, mode in keys:
        rs = [r for r in results if (r[0], r[1]) == (effect, mode) and r[4] and r[5]]
        if not rs: continue
        xs = [r[4] / r[5] for r in rs]
        say("  %s %s: faster on %d of %d settings, x %.2f to %.2f" % (
            effect, mode if mode != "-" else "", sum(x >= 1 for x in xs), len(xs), min(xs), max(xs)))
        for r in sorted(rs, key=lambda r: r[4] / r[5])[:3]:
            if r[4] / r[5] < 1:
                say("      slower: %s %s  (Sapphire %.0f ms, MillSaph %.0f ms)" % (
                    r[2], r[3], r[4], r[5]))

try:
    rows = [r for r in csv.DictReader(open(CSV))
            if (not ONLY or r["effect"] == ONLY) and MATCH in r["setting"]]
    # The parameters' image: the bench's reference (else made here, as before the images).
    source = os.path.join(IMAGES, "reference.exr")
    if not os.path.exists(source):
        source = os.path.join(HERE, "src_4k.exr")
    if not os.path.exists(source):
        k = nuke.nodes.Constant(color=[0, 0, 0, 1]); created.append(k)
        e = nuke.nodes.Expression(inputs=[k]); created.append(e)
        for c in range(3):
            plain = "0.2+0.5*x/width*%d/3+0.2*y/height" % (c + 1)
            e["expr%d" % c].setValue("(%s ? %g : %s ? %g : %s)+%s" % (
                DISC, 0.9 - 0.3 * c, BOX, 0.1 + 0.2 * c, plain, NOISE % (c + 1)))
        e["expr3"].setValue("1")
        nuke.execute(write(e, source), 1, 1)
    r = nuke.nodes.Read(file=source); r["raw"].setValue(True); created.append(r)
    g = nuke.nodes.Grade(inputs=[r]); created.append(g)
    nuke.startPerformanceTimers()
    say("Nuke %s, %d settings from %s, ms in the node per 4K frame" % (
        nuke.NUKE_VERSION_STRING, len(rows), CSV))
    try:
        probe = create(NODES["MS_BandPass"][1])
        nuke.delete(probe)
    except Exception:
        diagnose()
        raise RuntimeError("stopped: MillSaph's nodes are missing (see above)")
    say("Licence: " + licence_check(g))
    # Reading the image and writing the result, without an effect: taken off whole-frame times.
    noop = nuke.nodes.NoOp(inputs=[g]); created.append(noop)
    base = timed(noop, g, frames_only=True)[0]
    nuke.delete(noop); created.remove(noop)
    say("Reading + writing a frame without effect: %.0f ms" % base)
    results, framed = [], 0
    say("%-18s %-14s %-18s %-16s %9s %9s %8s %9s" % (
        "effect", "mode", "parameter", "value", "Sapphire", "MillSaph", "x", "MS effect"))
    for row in rows:
        classes = NODES.get(row["effect"])
        if not classes:
            continue
        times, notes, own = [], [], None
        # The Source: the test image the row names, else the reference.
        named = re.search(r"(?:^| )image=(\S+)", row["setting"])
        src = picture(named.group(1)) if named else g
        for vendor, cls in zip(("Sapphire", "MillSaph"), classes):
            try:
                n = create(cls)
            except Exception as ex:
                times.append(None); notes.append("%s: no node" % vendor); continue
            created.append(n)
            n.setInput(0, src)
            try:
                missing = apply(n, row["setting"], src)
                if missing:
                    notes.append("%s has no %s" % (vendor, ", ".join(missing)))
                if vendor == "MillSaph":
                    ms, how, own = timed(n, src, log=True)
                else:
                    ms, how = timed(n, src)
                times.append(ms)
                if how:
                    framed += 1
                    notes.append("%s: whole frame minus reading + writing" % vendor)
            except Exception as ex:
                times.append(None); notes.append("%s: %s" % (vendor, ex))
            nuke.delete(n); created.remove(n)
        s, m = times
        results.append((row["effect"], row["mode"], row["parameter"], row["value"], s, m, own))
        x = s / m if s and m else None
        cell = lambda v: "%9.0f" % v if v is not None else "%9s" % "-"
        say("%-18s %-14s %-18s %-16s %s %s %8s %s  %s" % (
            row["effect"], row["mode"], row["parameter"], row["value"], cell(s), cell(m),
            "%.2f" % x if x else "-", cell(own), "; ".join(notes)))
        out.write('%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' % (
            row["effect"], row["mode"], row["parameter"], row["value"],
            "%.1f" % s if s else "", "%.1f" % m if m else "", "%.3f" % x if x else "",
            "%.1f" % own if own else "", "; ".join(notes)))
        out.flush()
    say("(x: how many times faster MillSaph is. Sapphire, MillSaph: the time Nuke counts in the "
        "node, its own handling of the images included. MS effect: MillSaph's own time for the "
        "effect, from its log: the difference with MillSaph's column is Nuke's handling, about the "
        "same for Sapphire's node.)")
    if framed:
        say("%d times without Nuke's timers: whole frames minus reading + writing a frame "
            "without effect (%.0f ms), approximate." % (framed, base))
    summary(results)
    if os.path.exists(PLUGIN_LOG):  # where MillSaph ran: the lines other than each render's
        with open(PLUGIN_LOG) as f:
            f.seek(log_start)
            lines = [l for l in f.read().splitlines()
                     if " render " not in l and " settings " not in l]
        say("\nMillSaph's log during the test (%s), renders left out:\n%s" % (
            PLUGIN_LOG, "\n".join(lines)))
finally:
    try: nuke.stopPerformanceTimers()
    except Exception: pass
    for n in created:
        try: nuke.delete(n)
        except Exception: pass
    text.close()
    out.close()
