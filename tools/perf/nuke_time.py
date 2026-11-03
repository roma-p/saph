# Times every MillSaph effect at 4K in Nuke against its Sapphire counterpart, in one scene:
# the same image, settings at their defaults (EdgeAwareBlur: each Blur Type). The plugin must
# be installed in Nuke's OFX folder (e.g. /usr/OFX/Plugins on Linux). Run it in the Script
# Editor of an interactive Nuke (headless Nuke renders Sapphire unlicensed):
#
#   MILLSAPH_PERF_DIR = "~/millsaph_perf"         # optional: where the image and results go
#   exec(open("<MillSaph>/tools/perf/nuke_time.py").read())
#
# The test image (4K, photo-like, like tools/test_host.c --size) is rendered once to
# <dir>/src_4k.exr. Each timed render changes one setting by 1e-6, so that Nuke cannot reuse
# a cached result; the first render (GPU start-up) is not counted. The effect's own time
# comes from Nuke's performance timers (the time Nuke spent in the node), not from the whole
# frame minus reading and writing: Nuke does those alongside the effect. Results are printed
# and written to <dir>/timing.txt, followed by the lines MillSaph wrote meanwhile to its log
# (~/millsaph.log: the API it runs on, and where each render's time goes).
#
# A new effect gets a line in EFFECTS (and a case in perf.sh).
import os, time, nuke

HERE = os.path.expanduser(globals().get("MILLSAPH_PERF_DIR", "~/millsaph_perf"))
REPS = 6
SAPPHIRE = "OFXcom.genarts.sapphire."
MILLSAPH = "OFXcom.xiaoluo.millsaph."
# label, Sapphire node, MillSaph node, setting changed between renders, settings per case
EFFECTS = [
    ("BandPass", SAPPHIRE + "Stylize.S_BandPass_v1", MILLSAPH + "BandPass_v1", "brightness",
     [("", {})]),
    ("EdgeAwareBlur", SAPPHIRE + "BlurSharpen.S_EdgeAwareBlur_v1", MILLSAPH + "EdgeAwareBlur_v1",
     "edge_threshold",
     [("Sharp", {"blur_type": 0}), ("Soft", {"blur_type": 1}), ("Softer", {"blur_type": 2})]),
    ("Distort", SAPPHIRE + "Distort.S_Distort_v1", MILLSAPH + "Distort_v1", "amount", [("", {})]),
]
# The photo-like image: gradients, a disc and a box with hard edges, a little noise.
DISC = "pow(x/width-0.39,2)*256/81+pow(y/height-0.46,2)<0.077"
BOX = "x>width*0.68&&x<width*0.88&&y>height/7&&y<height*5/6"
NOISE = "(random(x,y,%d)-0.5)*0.04"

if not os.path.isdir(HERE):
    os.makedirs(HERE)
log = open(os.path.join(HERE, "timing.txt"), "w")
PLUGIN_LOG = os.path.expanduser("~/millsaph.log")  # src/common/log.h
plugin_log_start = os.path.getsize(PLUGIN_LOG) if os.path.exists(PLUGIN_LOG) else 0
def say(*a):
    s = " ".join(str(x) for x in a); print(s); log.write(s + "\n"); log.flush()

if not [f for f in nuke.formats() if f.name() == "MS_UHD"]:
    nuke.addFormat("3840 2160 1 MS_UHD")
nuke.root()["format"].setValue("MS_UHD")
created = []

def write(node, path):
    w = nuke.nodes.Write(inputs=[node], file=path, file_type="exr", datatype="32 bit float",
                         compression="none", channels="rgba")
    w["raw"].setValue(True)
    created.append(w)
    return w

# Best frame time of REPS renders after the first, in ms; all the frame times; and the
# node's own time per frame (Nuke's performance timers: best render after the first).
def timed(node, knob):
    w = write(node, os.path.join(HERE, "out.exr"))
    base = node[knob].value()
    times, own = [], []
    for rep in range(REPS):
        node[knob].setValue(base + rep * 1e-6)
        try: nuke.clearRAMCache()
        except Exception: pass
        nuke.resetPerformanceTimers()
        t = time.time(); nuke.execute(w, 1, 1); times.append((time.time() - t) * 1000)
        info = node.performanceInfo(nuke.PROFILE_ENGINE)
        own.append(float(info["timeTakenWall"]))  # ms
    node[knob].setValue(base)
    return min(times[1:]), times, min(own[1:])

try:
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
    r = nuke.nodes.Read(file=source); r["raw"].setValue(True)
    created.append(r)
    say("Nuke %s, image %d x %d" % (nuke.NUKE_VERSION_STRING, r.width(), r.height()))
    g = nuke.nodes.Grade(inputs=[r]); created.append(g)
    nuke.startPerformanceTimers()
    io_ms, _, _ = timed(g, "white")
    say("read + write without effect: %.0f ms per frame\n" % io_ms)

    summary = []  # (case, Sapphire ms, MillSaph ms)
    for label, sapphire, millsaph, knob, cases in EFFECTS:
        results = {}
        for vendor, cls in (("Sapphire", sapphire), ("MillSaph", millsaph)):
            try:
                try: n = nuke.createNode(cls, inpanel=False)
                except RuntimeError: n = nuke.createNode("OFX" + cls[3:].lower(), inpanel=False)  # Linux
            except Exception as ex:
                say("%s %s not available: %s" % (vendor, label, ex)); continue
            created.append(n)
            n.setInput(0, r)
            gpu = [(k.name(), k.value()) for k in n.allKnobs()
                   if "gpu" in k.name().lower() or "device" in k.name().lower()]
            say("%s %s  gpu settings: %s" % (vendor, label, gpu or "none"))
            for case, settings in cases:
                for name, value in settings.items():
                    n[name].setValue(value)
                ms, all_, own = timed(n, knob)
                results[(case, vendor)] = own
                say("  %-8s %6.0f ms per frame, in the node %6.0f ms   frames: %s" % (
                    case or "-", ms, own, " ".join("%.0f" % x for x in all_)))
        for case, _ in cases:
            summary.append(("%s %s" % (label, case), results.get((case, "Sapphire")),
                            results.get((case, "MillSaph"))))

    say("\nTime in the node (Nuke's performance timers), ms per 4K frame:")
    say("  %-22s %9s %9s %10s" % ("", "Sapphire", "MillSaph", "MillSaph x"))
    for case, s, m in summary:
        cell = lambda v: "%9.0f" % v if v is not None else "%9s" % "-"
        speed = "%9.2fx" % (s / m) if s and m and m > 0 else "%10s" % "-"
        say("  %-22s %s %s %s" % (case.strip(), cell(s), cell(m), speed))
    say("(MillSaph x: how many times faster than Sapphire.)")
    if os.path.exists(PLUGIN_LOG):
        with open(PLUGIN_LOG) as f:
            f.seek(plugin_log_start)
            say("\nMillSaph's log during the test (%s):\n%s" % (PLUGIN_LOG, f.read().rstrip()))
finally:
    try: nuke.stopPerformanceTimers()
    except Exception: pass
    for n in created:
        try: nuke.delete(n)
        except Exception: pass
    log.close()
