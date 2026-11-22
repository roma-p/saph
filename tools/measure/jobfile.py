# What the measure tools share: jobs files (nuke_measure.py's format), the names of a job's
# frames and their renders, and the tools' help (their header comment).
import argparse
import json
import os


def load(path):
    """A jobs file, its jobs by name in ["by_name"]."""
    jobs = json.load(open(os.path.expanduser(path)))
    jobs["by_name"] = {j["name"]: j for j in jobs.get("jobs", [])}
    return jobs


def frame_names(job):
    """A job's file names: <name>.exr, or <name>.####.exr over its "frames"."""
    first, last = job.get("frames", [1, 1])
    if first == last:
        return [job["name"] + ".exr"]
    return ["%s.%04d.exr" % (job["name"], f) for f in range(first, last + 1)]


def frame_pairs(jobs, job):
    """Per frame of `job`: (the reference's render, MillSaph's), in the jobs file's "out"."""
    out = os.path.expanduser(jobs["out"])
    return [(os.path.join(out, "reference", n), os.path.join(out, "millsaph", n))
            for n in frame_names(job)]


def parser(path):
    """An argument parser whose help is the header comment of the tool at `path`."""
    lines = []
    for line in open(path):
        if not line.startswith("#"):
            break
        lines.append(line[2:].rstrip() if line.startswith("# ") else line[1:].rstrip())
    return argparse.ArgumentParser(description="\n".join(lines),
                                   formatter_class=argparse.RawDescriptionHelpFormatter)
