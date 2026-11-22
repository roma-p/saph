# MillSaph's renders without Nuke: the jobs of a jobs file (nuke_measure.py's format) rendered
# by build/test_host, the same pixels as nuke_measure.py's MillSaph renders (the installed
# plugin there; here build/'s, nothing to install).
#
#   python3 tools/measure/ms_render.py <jobs.json> [job ...] [--out folder] [--cpu]
#
# writes <out>/<job>.exr (sequences: <job>.####.exr over "frames"), <out> by default the jobs
# file's "out" + "/millsaph". Knobs as in jobs files: numbers, lists, true / false, choice names
# or indices, texts (a lens, a style), animated {"keys": [[frame, value], ...]} (linear).
#
# As a module, to fit a setting by rendering MillSaph over a range of it:
#
#   import ms_render
#   jobs = ms_render.load("~/millsaph_measure/jobs/lensflare_28.json")
#   image = ms_render.render(jobs, jobs["by_name"]["lens_rs1"])            # top row first
#   image = ms_render.render(jobs, job, knobs={"lens_": edited_text}, time=3)
#   best, scores = ms_render.sweep(jobs, job, reference, values, lambda v: {...knobs...})
#
# Not on Windows (test_host: dlopen). The inputs: the jobs' probes (EXR) at the jobs' size.
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

sys.dont_write_bytecode = True  # (no __pycache__ in the repo)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import exr  # noqa: E402
import jobfile  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "build", "test_host")
PLUGIN = os.path.join(ROOT, "build", "MillSaph.ofx.bundle", "Contents",
                      "MacOS" if sys.platform == "darwin" else "Linux-x86-64", "MillSaph.ofx")
ID = "com.xiaoluo.millsaph."
# The reference's inputs that MillSaph's effects order differently (nuke_measure.py's INPUTS):
# the job's input index -> MillSaph's (None: none).
INPUTS = {"Glow": {1: 2, 2: 1}, "Glint": {1: None, 2: 1}, "Glare": {1: None, 2: 1}}

_described = {}


def describe(effect, plugin=PLUGIN):
    """The effect's inputs (in order), its choices' options and its widths (parameters of a
    normalised type: "x", "y"), from test_host."""
    if effect not in _described:
        out = subprocess.run([HOST, plugin, ID + effect, "--size", "8x8"], capture_output=True,
                             text=True).stdout
        clips = [c for c in re.findall(r"^  clip (\S+)$", out, re.M) if c != "Output"]
        choices = {}
        for name, k, option in re.findall(r'^    (\S+) OfxParamPropChoiceOption\[(\d+)\] = "(.*)"$',
                                          out, re.M):
            choices.setdefault(name, {})[option] = int(k)
        # (the hosts give these as fractions of the project's width or height; jobs as Nuke
        # shows them, in pixels)
        widths = {name: "y" if kind.endswith("NormalisedY") else "x"
                  for name, kind in re.findall(r'^    (\S+) OfxParamPropDoubleType\[0\] = '
                                               r'"OfxParamDoubleType(Normalised[XY])"$', out, re.M)}
        _described[effect] = (clips, choices, widths)
    return _described[effect]


load = jobfile.load  # (ms_render.load, as before)


def at_time(value, time):
    """An animated knob's value at `time` (its keys, linear between them, held outside)."""
    if not (isinstance(value, dict) and "keys" in value):
        return value
    keys = sorted(value["keys"], key=lambda k: k[0])
    frames = [k[0] for k in keys]
    vals = [np.atleast_1d(np.asarray(k[1], dtype=np.float64)) for k in keys]
    if time <= frames[0]:
        v = vals[0]
    elif time >= frames[-1]:
        v = vals[-1]
    else:
        i = max(j for j in range(len(frames)) if frames[j] <= time)
        t = (time - frames[i]) / (frames[i + 1] - frames[i])
        v = vals[i] + (vals[i + 1] - vals[i]) * t
    return v.tolist() if len(v) > 1 else float(v[0])


def render(jobs, job, knobs=None, time=1, cpu=False, plugin=PLUGIN):
    """Job `job` of `jobs` (load) rendered by MillSaph at frame `time`, its knobs updated by
    `knobs`: RGBA floats, top row first (as exr.read)."""
    effect = job["effect"]
    width, height = jobs["size"]
    clips, choices, widths = describe(effect, plugin)
    all_knobs = dict(job.get("knobs", {}))
    all_knobs.update(knobs or {})
    with tempfile.TemporaryDirectory() as tmp:
        args = [HOST, plugin, ID + effect, "--size", "%dx%d" % (width, height), "--time", str(time)]
        for k, v in all_knobs.items():
            v = at_time(v, time)
            if isinstance(v, str) and k in choices:
                v = choices[k][v]
            if k in widths and isinstance(v, (int, float)) and not isinstance(v, bool):
                v = v / (width if widths[k] == "x" else height)
            if isinstance(v, str):  # a text: from a file
                path = os.path.join(tmp, k + ".txt")
                open(path, "w").write(v)
                args.append("%s=@%s" % (k, path))
            elif isinstance(v, bool):
                args.append("%s=%d" % (k, v))
            elif isinstance(v, (list, tuple)):
                args.append("%s=%s" % (k, ",".join("%.17g" % x for x in v)))
            else:
                args.append("%s=%.17g" % (k, v))
        given = set()
        probes = os.path.expanduser(jobs.get("probes", "~/millsaph_measure/probes"))
        for i, image in enumerate(job.get("inputs", [])):
            i = INPUTS.get(effect, {}).get(i, i)
            if image is None or i is None or i >= len(clips):
                continue
            name = image.replace("####", "%04d" % time)
            pixels = np.asarray(exr.read(os.path.join(probes, name + ".exr")), dtype=np.float32)
            if pixels.shape[:2] != (height, width):
                raise ValueError("%s is %dx%d, the jobs %dx%d" % (name, pixels.shape[1],
                                                                  pixels.shape[0], width, height))
            if pixels.shape[2] < 4:  # (RGB: alpha 1)
                pixels = np.concatenate([pixels, np.ones((height, width, 1), np.float32)], 2)
            path = os.path.join(tmp, "in%d.raw" % i)
            np.ascontiguousarray(pixels[::-1, :, :4]).tofile(path)
            args += ["--image", "%s=%s" % (clips[i], path)]
            given.add(clips[i])
        args += ["!" + c for c in clips if c not in given]
        out = os.path.join(tmp, "out.raw")
        env = dict(os.environ, CUDA_VISIBLE_DEVICES="") if cpu else None
        r = subprocess.run(args + ["--save", out], capture_output=True, text=True, env=env)
        if r.returncode != 0 or not os.path.exists(out):
            message = re.findall(r"message \(\w+\): (.*)", r.stdout)
            raise RuntimeError("%s: %s" % (job["name"], message[-1] if message else "failed"))
        return np.fromfile(out, dtype=np.float32).reshape(height, width, 4)[::-1]


def sweep(jobs, job, reference, values, knobs_for, time=1, channel=1):
    """MillSaph rendered at each of `values` (knobs_for(value): the knobs to change) against
    `reference` (an image, top row first): the value with the least |difference| (its sum over
    `channel`), refined between its neighbours (a parabola), and every (value, difference)."""
    scores = []
    for v in values:
        image = render(jobs, job, knobs_for(v), time)
        scores.append((v, float(np.abs(image[..., channel] - reference[..., channel]).sum())))
    k = min(range(len(scores)), key=lambda i: scores[i][1])
    best = scores[k][0]
    if 0 < k < len(scores) - 1:
        (x0, y0), (x1, y1), (x2, y2) = scores[k - 1:k + 2]
        den = y0 - 2 * y1 + y2
        if den > 0 and abs(x2 - x1 - (x1 - x0)) < 1e-9:  # (evenly spaced)
            best = x1 + 0.5 * (y0 - y2) / den * (x1 - x0)
    return best, scores


def main(argv):
    p = jobfile.parser(__file__)
    p.add_argument("jobs")
    p.add_argument("job", nargs="*")
    p.add_argument("--out")
    p.add_argument("--cpu", action="store_true")
    args = p.parse_intermixed_args(argv)
    cpu = args.cpu
    jobs = load(args.jobs)
    names = args.job or [j["name"] for j in jobs["jobs"]]
    out = os.path.expanduser(args.out or os.path.join(jobs["out"], "millsaph"))
    os.makedirs(out, exist_ok=True)
    failed = 0
    for name in names:
        job = jobs["by_name"][name]
        frames = job.get("frames", [1, 1])
        try:
            for f in range(frames[0], frames[1] + 1):
                seq = ".%04d" % f if frames[0] != frames[1] else ""
                exr.write(os.path.join(out, name + seq + ".exr"), render(jobs, job, time=f, cpu=cpu))
            print("%-32s ok" % name)
        except Exception as ex:  # (a job that fails: said, the others rendered)
            print("%-32s FAILED: %s" % (name, ex))
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
