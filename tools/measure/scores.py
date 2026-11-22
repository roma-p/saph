# How close MillSaph's renders of a batch are to the reference's, job by job:
#
#   python3 tools/measure/scores.py <jobs.json> [--save scores.json] [--diff old.json] [--all]
#
# For each job (a sequence: its worst frame), from <out>/reference and <out>/millsaph:
#   p99.9   the 99.9th percentile of |millsaph - reference| / the reference's peak (a black
#           reference: 0.05 x MillSaph's peak), RGB;
#   L1      sum |millsaph - reference| / sum reference;
#   energy  sum millsaph / sum reference.
# Prints the worst jobs first (the 25 worst; --all: every one), then how many are within 1, 2,
# 5, 10 and 20 % and the median. --save writes them (JSON, by job); --diff compares with saved
# ones: the jobs that got worse or better by more than 0.002, and both medians.
import json
import os
import sys

import numpy as np

sys.dont_write_bytecode = True  # (no __pycache__ in the repo)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exr  # noqa: E402
import jobfile  # noqa: E402
from jobfile import frame_names  # noqa: E402,F401 (scores.frame_names, as before)


def score(reference, millsaph):
    a = reference[..., :3].astype(np.float64)
    m = millsaph[..., :3].astype(np.float64)
    peak = max(np.abs(a).max(), 0.05 * np.abs(m).max(), 1e-12)
    d = np.abs(m - a)
    total = max(np.abs(a).sum(), 1e-12)
    return float(np.percentile(d / peak, 99.9)), float(d.sum() / total), float(m.sum() / total)


def batch_scores(jobs_path):
    jobs = jobfile.load(jobs_path)
    result = {}
    for job in jobs["jobs"]:
        worst = None
        for ref, ms in jobfile.frame_pairs(jobs, job):
            if not (os.path.exists(ref) and os.path.exists(ms)):
                worst = None
                break
            a, m = exr.read(ref), exr.read(ms)
            if a.shape != m.shape:  # (e.g. a setting MillSaph does not have that enlarges it)
                worst = None
                break
            s = score(a, m)
            worst = s if worst is None or s[0] > worst[0] else worst
        if worst:
            result[job["name"]] = {"p999": worst[0], "l1": worst[1], "energy": worst[2]}
    return result


def summary(values):
    v = np.array(values)
    within = " ".join("<=%g%%: %.0f%%" % (t * 100, 100 * (v <= t).mean())
                      for t in (0.01, 0.02, 0.05, 0.1, 0.2))
    return "median %.2f%%; %s" % (100 * np.median(v), within)


def main(argv):
    p = jobfile.parser(__file__)
    p.add_argument("jobs")
    p.add_argument("--save")
    p.add_argument("--diff")
    p.add_argument("--all", action="store_true")
    args = p.parse_args(argv)
    scores = batch_scores(args.jobs)
    if not scores:
        print("no renders to compare (both reference/ and millsaph/ are needed)")
        return 1
    rows = sorted(scores.items(), key=lambda kv: -kv[1]["p999"])
    for name, s in rows if args.all else rows[:25]:
        print("  p99.9 %7.2f%%  L1 %7.2f%%  energy %.3f  %s" % (100 * s["p999"], 100 * s["l1"],
                                                               s["energy"], name))
    print("%d jobs: %s" % (len(rows), summary([s["p999"] for _, s in rows])))
    if args.save:
        json.dump(scores, open(args.save, "w"), indent=1)
    if args.diff:
        old = json.load(open(args.diff))
        both = [(scores[k]["p999"] - old[k]["p999"], k) for k in scores if k in old]
        worse = sorted((d, k) for d, k in both if d > 0.002)[::-1]
        better = sorted((d, k) for d, k in both if d < -0.002)
        for title, items in (("worse", worse), ("better", better)):
            print("%s: %d" % (title, len(items)))
            for d, k in items[:15]:
                print("  %+7.2f%%  %s (%.2f%% -> %.2f%%)" % (100 * d, k, 100 * old[k]["p999"],
                                                            100 * scores[k]["p999"]))
        print("median before %.2f%%, after %.2f%%" % (
            100 * np.median([old[k]["p999"] for _, k in both]),
            100 * np.median([scores[k]["p999"] for _, k in both])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
