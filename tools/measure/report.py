# Every effect against the reference at once: MillSaph's renders of every measurement batch
# (~/millsaph_measure/jobs, rendered headless by ms_render.py) scored against the reference's
# renders (scores.py), summed up per effect.
#
#   python3 tools/measure/report.py [--render] [--cpu] [--save report.json] [effect ...]
#
# --render renders MillSaph's images again first (build/'s plugin; --cpu: on the CPU, faster to
# start for many small renders); else the images already there are scored. Per effect: the
# jobs scored, the median of their p99.9 and L1 (scores.py), the share within 1% and 5%
# (p99.9). A batch is the jobs file whose "out" it is; when a <batch>_ms.json renders MillSaph's
# side (an older batch's knobs for MillSaph), that one is used.
import glob, json, os, subprocess, sys
import numpy as np

sys.dont_write_bytecode = True  # (no __pycache__ in the repo)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jobfile  # noqa: E402
import scores  # noqa: E402

HOME = os.path.expanduser("~/millsaph_measure")
HERE = os.path.dirname(os.path.abspath(__file__))


def batches():
    """{effect: [jobs file, ...]}, one file per batch (out folder)."""
    by_out = {}
    for path in sorted(glob.glob(os.path.join(HOME, "jobs", "*.json"))):
        spec = jobfile.load(path)
        if not spec.get("jobs"):
            continue
        out = os.path.basename(os.path.expanduser(spec["out"]).rstrip("/"))
        name = os.path.basename(path)[:-5]
        # (the batch's own file, or its _ms one; variants such as lensflare_23y_ms left out)
        if name not in (out, out + "_ms"):
            continue
        if name == out + "_ms" or out not in by_out:
            by_out[out] = (path, spec["jobs"][0]["effect"])
    result = {}
    for out, (path, effect) in sorted(by_out.items()):
        result.setdefault(effect, []).append(path)
    return result


def main(argv):
    p = jobfile.parser(__file__)
    p.add_argument("effect", nargs="*")
    p.add_argument("--render", action="store_true")
    p.add_argument("--cpu", action="store_true")
    p.add_argument("--save")
    args = p.parse_intermixed_args(argv)
    report = {}
    for effect, paths in batches().items():
        if args.effect and effect not in args.effect:
            continue
        values = {}
        for path in paths:
            if args.render:
                run = [sys.executable, os.path.join(HERE, "ms_render.py"), path]
                if args.cpu:
                    run.append("--cpu")
                subprocess.run(run, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            batch = os.path.basename(path)[:-5]
            for job, s in scores.batch_scores(path).items():
                values[batch + "/" + job] = s
        if not values:
            continue
        p = np.array([s["p999"] for s in values.values()])
        l1 = np.array([s["l1"] for s in values.values()])
        report[effect] = {"jobs": len(values), "batches": len(paths),
                          "p999_median": float(np.median(p)), "l1_median": float(np.median(l1)),
                          "within_1": float((p <= 0.01).mean()), "within_5": float((p <= 0.05).mean())}
        r = report[effect]
        print("%-18s %4d jobs %2d batches  p99.9 median %6.2f%%  L1 median %6.2f%%  "
              "<=1%%: %3.0f%%  <=5%%: %3.0f%%" % (effect, r["jobs"], r["batches"],
                                                100 * r["p999_median"], 100 * r["l1_median"],
                                                100 * r["within_1"], 100 * r["within_5"]),
              flush=True)
    if args.save:
        json.dump(report, open(args.save, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
