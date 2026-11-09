# Measuring an effect to recreate it

MillSaph's effects reproduce, as closely as possible, existing OFX effects (the "reference
plugin"), from their renders only: what an effect does is found by rendering chosen images
through it in Nuke and fitting a model to the results. Nothing comes from the reference
plugin's binary (no disassembly, no strings, no hooking), only from its renders, its
parameters as Nuke shows them, and its public documentation.

This folder has the tools; `HANDOFF.md` has the working rules of the project.

## Tools

| File | What |
|---|---|
| `nuke_measure.py` | In Nuke. `describe`: every knob of an effect's nodes (default, range, choices, tooltip). `jobs`: renders a list of cases with the reference node and MillSaph's. Node classes of each effect: its `EFFECTS` table. |
| `probes.py` | Writes the probe images (impulse, step, ramps, levels, colours, noise, alpha, checker) as EXR. |
| `compare.py` | Compares two EXR files or folders: largest and mean difference, where. |
| `exr.py` | Reads and writes uncompressed EXR with numpy (no OpenEXR module). |

Everything goes through `~/millsaph_measure/` (in the home folder: Nuke's interface may not
see a terminal's `/tmp`):

```
~/millsaph_measure/
  probes/            probes.py's images (and any other input image, EXR, uncompressed)
  describe/          nuke_measure.py describe
  jobs/              the jobs files (JSON)
  renders/<batch>/   reference/, millsaph/, jobs.log
  <effect>/          the model (model.py) and notes of each effect
```

The Python tools need numpy (`python3 -m pip install --user numpy` if missing).

## Licence check

The reference plugin draws a watermark when it has no licence (black and a red cross), and
Nuke started without its interface (`nuke -t`) may not find the licence. Every `jobs` run
first compares its BandPass with MillSaph's (the same to 1e-5) and prints the result: a
"NOT LICENSED" line means its renders of that run are useless. If `nuke -t` is not
licensed, run the same jobs from the Script Editor of Nuke's interface:

```python
MILLSAPH_MEASURE = "jobs ~/millsaph_measure/jobs/glow_1.json"
exec(open("/path/to/MillSaph/tools/measure/nuke_measure.py").read())
```

MillSaph has to be installed in that Nuke (`OFX_PLUGIN_PATH=<MillSaph>/build`, or a copy of
`MillSaph.ofx.bundle` in the OFX folder; an old copy elsewhere, e.g. `/usr/OFX/Plugins`, can
hide the new one).

## Method

1. **Parameters first.** `nuke_measure.py describe <Effect>` gives the knobs, their defaults
   and ranges, the inputs. Read the effect's documentation page too (its purpose, the names
   of its options). Note which widths are normalised: a fraction of the project's width, shown
   in pixels by Nuke; typed values are converted with the current project width, so set the
   project format before typing or setting them.
2. **Probes, one thing at a time.** Render the probes at the defaults, then change one knob at
   a time over a few values (ends, middle, default). An impulse gives a linear filter's kernel
   directly; `impulse_off` shows whether it depends on the level; `step` gives edges and
   borders; `levels` and `colors` give per-level curves and channel mixing; `alpha_ramp`
   shows how alpha is used (straight, premultiplied, ignored). Keep images small (256 x 128):
   batches are fast and the numbers easy to read.
3. **A model in numpy** (`~/millsaph_measure/<effect>/model.py`), fitted to the renders, from
   plain building blocks first. What the effects measured so far turned out to be built from:
   - box blurs: three boxes horizontally then vertically, the rounded radius split into three
     near-equal integers (the remainder to the last boxes), borders mirrored;
   - luma = 5/16 R + 8/16 G + 3/16 B;
   - normalised widths in pixels = value x project width (x the per-axis "Rel" factors), then
     rounded;
   - masks mixing the source and the result by the mask's luma or alpha, often blurred like
     the effect, with an invert option;
   - alpha passed through unless the effect is about alpha;
   - recursive filters running over rows bottom-up (OFX order), which shows in exact
     comparisons.
   Then compare model and renders with `compare.py` and refine with focused batches on what
   still differs. The aim is float precision (about 1e-5 relative); when it stops there, say
   what is left and why (e.g. ties in integer windows) rather than forcing it.
4. **Effects with randomness** (noise, damage, grain): the random generator cannot be
   reproduced from renders. Match what can be measured (what each parameter changes, sizes,
   amounts, statistics, how it moves with the seed and over time) and check by eye with the
   user. **Time effects** (flicker, matching over frames): render sequences (inputs with
   `####`, `"frames"`), and check whether a frame depends on other frames.
5. **Implement in MillSaph** (Halide kernels, `HANDOFF.md`), then run the same jobs on
   MillSaph's node and compare: `compare.py renders/<batch>/reference renders/<batch>/millsaph`.
   Keep each effect's validation batch: it is the regression test of later speed work.
6. **Speed**: `tools/perf/` (bench cases, sweep, Nuke timings against the reference node).
