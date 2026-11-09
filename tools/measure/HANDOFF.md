# Handoff: recreating the next effects

For a new conversation (Claude Code) on a Linux machine with Nuke and the reference plugin
licensed, to do for a list of new effects what was done for BandPass, EdgeAwareBlur and
Distort: measure the reference effect, recreate it in MillSaph (Halide, CPU and GPU), check
it against the reference, then make it as fast. Read this file, then `tools/measure/README.md`
(the method and its tools), then the top-level `README.md` (build, layout).

Save the rules below in your memory at the start (the previous conversation's memory stays
on the Mac).

## Rules of the project (the user's)

- Talk to the user in French. Code, comments and docs in English.
- **Never name the reference plugin** (its product name, its vendors, its `S_` node names) in
  code, comments, READMEs or commit messages. Write "the reference" or describe the effect.
  Only the Nuke scripts (`tools/measure/nuke_measure.py`, `tools/perf/nuke_*.py`) name its
  node classes. Avoid "measured on" / "black-box" wording in the code: state the formula.
- Nothing from the reference plugin's binary: renders, Nuke knobs and public docs only.
- Commit only when the user asks. Stage files by explicit path (never `git add -A`); never
  stage the top-level `glsl/` folder.
- Build only with `./build.sh` (or `build.bat`), without arguments. Environment variables
  allowed for the build: `MILLSAPH_VERSION`, `MILLSAPH_THIRD_PARTY_DIR`,
  `MILLSAPH_OFX_VERSION` only.
- All effect maths in Halide (`src/halide/`); C (`src/effects/`, `src/engine/`) only
  orchestrates. No hand-written C, GLSL, CUDA or Metal kernels.
- Every effect runs on the GPU. A GPU bug is fixed on the GPU: falling back to the CPU is not
  a fix.
- Every effect has perf coverage (`tools/perf/`: bench cases, sweep, `nuke_sweep.py` /
  `nuke_time.py` entries against the reference node), like its validation batch.
- C style: `#pragma once`, `//` comments, malloc and free in the same function, identifiers
  through `MS_ID("X")`, includes naming their folder (`#include "engine/engine.h"`).
- Test data lives in the home folder (`~/millsaph_measure`, `~/millsaph_perf`), not `/tmp`.
- Reports to the user are short, with numbers (times, differences).

## The effects to do

The user's list, as nodes of the reference plugin (`EFFECTS` in `nuke_measure.py` has their
classes). Two names are guesses: confirm them with the user before starting on them.

| Effect | Note |
|---|---|
| EdgeBlur | listed twice by the user ("edgeblur", "edge blur"): one effect |
| RackDefocus | defocus with an iris shape |
| Glow | |
| Glint | |
| Glare | its styles may be presets: find out which knobs they set |
| LensFlare | the user wrote "flare": confirm. Large (flare presets and elements) |
| Flicker | time effect |
| FlickerMatch | time effect, matches a reference region's level |
| FlickerMatchColor | the user wrote "color match": there is no node of that name, this is the guess; confirm |
| TVDamage | random |
| JpegDamage | block DCT |
| PixelSort | |
| Mosaic | |

Suggested order, simplest and most deterministic first, so that the method and the shared
Halide helpers settle before the large ones: Mosaic, Glow, EdgeBlur, RackDefocus, Glint, Glare,
JpegDamage, PixelSort, FlickerMatch, FlickerMatchColor, Flicker, TVDamage, LensFlare. Agree on
it with the user.

There were earlier, unmeasured guesses of some of these as Nuke Matchbox shaders on the Mac
(not in this repository): ignore them; start from the renders.

## Steps per effect

1. `nuke -t tools/measure/nuke_measure.py describe <Effect>` (describing works without the
   reference's licence) and read `~/millsaph_measure/describe/<Effect>.txt`.
2. `python3 tools/measure/probes.py` once; write jobs files in `~/millsaph_measure/jobs/`;
   render them (`nuke -t ... jobs <file>`, or the Script Editor when headless is not licensed:
   the first line of every run says). Ask the user to run the Script Editor batches when
   needed: give them the two lines to paste.
3. Fit a numpy model (`~/millsaph_measure/<effect>/model.py`) until it matches the renders;
   note what is measured and what is a guess.
4. Implement: a generator in `src/halide/` (shared helpers: `blur.h` 3-box blurs, `mask.h`,
   `shared.h`), its `ms_kernel(...)` lines in `CMakeLists.txt` (CPU and GPU), the effect in
   `src/effects/ms_<effect>.c` (parameter and clip tables like `ms_bandpass.c`, its kernels run
   through `ms_run`), listed in `src/effects/effects.c`. Add its node class to `EFFECTS` in
   `nuke_measure.py` (the MillSaph column) and the `NODES` of `tools/perf/nuke_sweep.py`.
5. Check: the same jobs on MillSaph's node, `compare.py`; `millsaph_bench --dump` GPU against
   CPU; `tools/sanitize.sh` (add cases); `tools/tidy.sh`, `tools/format.sh --check`.
6. Speed: bench cases in `tools/perf/bench.c`, then `tools/perf/sweep_all.sh` and the Nuke
   sweep against the reference.

## What the first three effects taught

Measuring:
- Normalised widths are a fraction of the **project** width: the project format must be the
  one meant before any knob is set (a stale format once explained two "wrong" renders).
- Headless Nuke can render the reference unlicensed (watermark): always check the first line.
- On Rocky, Nuke 16 headless once did not see MillSaph (an old copy in `/usr/OFX/Plugins`,
  Nuke's OFX cache in `~/.nuke/ofxplugincache`).
- Nuke's performance timers sometimes give 0 for a render: drop those, as `nuke_sweep.py`.
- Exactness can stop at float order effects (ties in integer windows): explain, don't force.

Halide and the GPU (the full notes are in the code comments):
- Halide's default error handler aborts the process (it would kill the host):
  `ms_catch_errors()` in `engine/kernel.c` is installed before any call; keep it.
- Metal: the release runtime silently skips a dispatch above the threadgroup limit (wrong
  output, no error); always compare GPU and CPU at 4K, not only on small images.
- Shared memory with float4 and one-float arrays needs `align_storage(..., 4)` (a CUDA-only
  wrong image came from that).
- Images under 16 pixels wide or tall are refused (vectorize / split schedules).
- `select()` evaluates both sides: bounds must cover both.
- Metal has no double and `strict_float` is ignored there: long running sums need chunked,
  compensated sums (`shared.h`).
- A stride-0 (constant) image uploads one pixel at a time on CUDA: `ms_run` uploads it 1x1.
- Vulkan was dropped on Linux (slow, wrong on Distort): Linux is CUDA, macOS Metal, Windows
  CUDA then Direct3D 12.
- Hosts give 16-bit or 32-bit images: `ms_run` converts on the GPU (the effect only sees
  floats). With CUDA the host's images are page-locked for the copies (`engine/pin.c`).

Speed in Flame and Nuke:
- Flame scrubs at about 40 ms per HD frame or less: the whole effect has to stay on the GPU
  for every setting (the user found lags as soon as a knob sent part of an effect to the CPU).
- Transfers dominate on CUDA (about 2.7 ms per HD float image each way, page-locked): count
  them in the design (one upload per distinct input, keep intermediate images on the GPU).
- Effects that leave the image as it is for some settings declare it (`MsEffect.identity`):
  the host then skips them.

## Working with the user

- The user tests on Rocky Linux (RTX A6000, Nuke 14 / 16) and in Flame (16-bit images), on a
  Mac (Metal), and soon on Windows with After Effects. They run what you give them and paste
  the results back: give exact commands, and say what to look at.
- For another machine, they want a fresh archive of the source tree each time (not file by
  file): `millsaph_perf.zip` without `build/`, `build-sanitize/`, `third_party/` (except the
  After Effects SDK zips), `glsl/`, `.git/`; check it against the tree after making it.
- They decide on GPU work from real numbers against the reference: give times side by side.

## Open points left by the previous conversation

- Distort: GPU and CPU differ (0.13 at the defaults, up to 100 on HDR images): an algorithm
  difference to find; which side is right is not known yet (check both against the reference).
- CUDA first use: 0.3 to 1.3 s per kernel the first time in a session (JIT); check whether it
  stays across sessions (`~/.nv/ComputeCache`), else warm the kernels up at load.
- Windows (MSVC, `build.bat`) and After Effects: never built there yet.
