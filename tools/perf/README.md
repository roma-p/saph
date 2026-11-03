# Performance tests

## Everything at once: `collect.sh`

    tools/perf/collect.sh

Builds, then writes one report file, `millsaph_report_<host>_<date>.txt`, to send back as it
is:

- the machine (OS, CPU, memory, GPU, drivers, CUDA libraries) and the build log;
- `build/millsaph_bench` (`bench.c`): every effect at 4K and HD on every API built here (the
  CPU, then Metal or CUDA), each GPU result compared with the CPU's, and the
  choices tuned on another machine (`src/effects/profile.c`) tried both ways;
- `perf.sh` (below);
- Nuke's timing, if `nuke_time.py` ran on this machine before (see below: run it first).

Every effect has a case in `perf.sh` and a line in `nuke_time.py`: add them with each new
effect.

## Everything against the reference plugin: `sweep_all.sh`

    tools/perf/sweep_all.sh [effect]     # build, then the sweep below; says what to run in Nuke
    (Nuke's Script Editor: nuke_sweep.py, as it says)
    tools/perf/sweep_all.sh report       # millsaph_sweep_report_<host>_<date>.txt

In Nuke started as usual: Nuke started without its interface (`nuke -t`) may not find the
reference plugin's licence, and an unlicensed reference plugin does not render its effect (its
times then mean nothing). `nuke_sweep.py` first checks it: the reference BandPass must match
MillSaph's.

## Every setting: `millsaph_bench --sweep`, then `nuke_sweep.py`

    ./build.sh
    mkdir -p ~/millsaph_perf && cd ~/millsaph_perf
    <path to MillSaph>/build/millsaph_bench --sweep 3840 2160 3       # [effect], e.g. MS_Distort

Each parameter of each effect over a series of values taken from its description (its
slider's ends, quarter, middle and default; every entry of a menu; both states of a
checkbox), the others at their defaults; EdgeAwareBlur for each Blur Type; Mask parameters
with the Mask connected; each optional input once. Each setting renders as in a host (the
column `on` says which API took it: a GPU, or the CPU when the effect declines the GPU for that
setting) and on the CPU, and the two results are compared. New effects are covered without
any change. Written to the terminal and to `millsaph_sweep.csv` (about 10 minutes at 4K).

Then the images: each effect (each Blur Type) at its defaults on each test image, and with
particular Mask and Lens / Edge Source images. The images are generated (`tools/perf/images.c`)
and written to `~/millsaph_perf/images/` as EXR (about 700 MB at 4K), where Nuke reads them
too: both time the same pixels.

| Image | What |
|---|---|
| `reference` | gradients, a disc and a box, a little noise: the parameters' image |
| `plate` | like a plate: fractal light and colour, rough ground, objects with hard edges, grain, highlights |
| `flat` | one grey everywhere |
| `noise` | white noise, 0 to 1 |
| `denormal` | values below 1e-38 (denormal floats: slow on some CPUs) |
| `hdr` | sky 1 to 20, a sun of 500, speculars up to 100 |
| `alpha` | the plate premultiplied: alpha 0 on a third, a soft disc |
| `nan_inf` | the reference with a grid of NaN and infinite pixels |
| `mask_hdr` | a Mask from 0 to 4 (larger EdgeAwareBlur windows) |
| `mask_sparse` | a Mask black but for a small disc |

Real plates: put them in `~/millsaph_perf/images/` as EXR at the sweep's size (3840 x 2160),
uncompressed (Nuke's Write: compression none, 16 or 32 bit), any name. Each is then a Source
image of the sweep, in the bench and in Nuke.

Then, in Nuke's Script Editor, the same settings on MillSaph's node and on the reference plugin's:

    exec(open("<path to MillSaph>/tools/perf/nuke_sweep.py").read())

Results in `~/millsaph_perf/sweep_nuke.txt` (and `.csv`): per setting, the time Nuke counts in
each node (its own handling of the images included, about the same for both), and in
`MS effect` MillSaph's own time for the effect, from its log. Renders where Nuke's timers give 0
are left out; a setting where they give nothing is timed on the whole frame minus reading +
writing, and says so. One effect only:
`MILLSAPH_SWEEP_EFFECT = "MS_EdgeAwareBlur"` before the `exec`. Over 200 settings take a long
time in Nuke (each one renders 4 times on each node). Only some: `MILLSAPH_SWEEP_MATCH = "input="` (a part of the CSV's `setting` column).

## CUDA copy speeds: `millsaph_bench --transfer`

    build/millsaph_bench --transfer [width height]     # default 1920 1080

How fast an RGBA float image goes to the GPU and back, three ways: from the host's memory as
it is (what the engine does today), with that memory page-locked for the copy, and through
page-locked memory of our own. Linux, CUDA only.

## Outside any host: `perf.sh`

    ./build.sh
    tools/perf/perf.sh

Renders every effect (BandPass, EdgeAwareBlur Sharp / Soft / Softer, Distort on the GPU and
on the CPU) at 3840 x 2160 with `tools/test_host.c`, 6 times each, and prints the best time
after the first render. The effects print the GPU API they really run on:

    [MillSaph] running on CUDA

or, when an API fails, `[MillSaph] CUDA not available (Halide error ...), trying the next`
(then the CPU). EdgeAwareBlur Soft runs on the GPU where `src/effects/profile.c`
says so (not on Metal: the M1 Pro's CPU is faster); the bench's `EAB Soft gpu` / `EAB Soft cpu`
cases compare the two.

## In Nuke, against the reference plugin: `nuke_time.py`

1. Install the plugin where Nuke finds OFX plugins (Linux: `/usr/OFX/Plugins`, macOS:
   `/Library/OFX/Plugins`), replacing an older copy, then restart Nuke:

       sudo cp -R build/MillSaph.ofx.bundle /usr/OFX/Plugins/

2. Start Nuke, and in its Script Editor run:

       exec(open("<path to MillSaph>/tools/perf/nuke_time.py").read())

It times BandPass, EdgeAwareBlur (each Blur Type) and Distort, the reference plugin's node then
MillSaph's, on the same 4K image with default settings. Results go to
`~/millsaph_perf/timing.txt`, with a table of each effect's own time (Nuke's performance
timers: the time spent in the node) and how many times faster MillSaph is, then MillSaph's own
log lines of the test: the API it ran on and, per render, where the time went (fetching the
images from Nuke, copying, the effect itself). Headless Nuke (`nuke -t`) cannot be used: the
reference plugin renders unlicensed there.
