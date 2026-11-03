#!/bin/sh
# The whole comparison with the reference plugin, in two steps around Nuke (its licence is
# only found in Nuke started as usual, with its interface):
#
#   tools/perf/sweep_all.sh [effect]    1. builds, sweeps every parameter of every effect
#                                          outside any host (build/millsaph_bench --sweep), and
#                                          says what to run in Nuke
#   (in Nuke's Script Editor)           2. tools/perf/nuke_sweep.py: the same settings on
#                                          MillSaph's and the reference plugin's nodes
#   tools/perf/sweep_all.sh report      3. puts both in millsaph_sweep_report_<host>_<date>.txt,
#                                          in the MillSaph folder
#
# effect: only this one, e.g. MS_EdgeAwareBlur. Everything goes through ~/millsaph_perf, in the
# home folder: Nuke may not see the same /tmp as a terminal; the test images in
# ~/millsaph_perf/images (about 700 MB; real plates can be added there, see tools/perf/README.md).

cd "$(dirname "$0")/../.." || exit 1
root=$(pwd)
out="$HOME/millsaph_perf"
mkdir -p "$out"

section() { printf '\n==== %s\n' "$1"; }

if [ "${1:-}" = report ]; then
    report="$root/millsaph_sweep_report_$(hostname -s)_$(date +%Y%m%d_%H%M).txt"
    {
        section "machine"
        uname -a
        command -v nvidia-smi > /dev/null && nvidia-smi --query-gpu=name,driver_version --format=csv
        section "sweep outside any host, 4K (millsaph_bench --sweep)"
        cat "$out/sweep_bench.txt" 2> /dev/null || echo "(not run: tools/perf/sweep_all.sh first)"
        section "the same settings in Nuke, MillSaph against the reference plugin (nuke_sweep.py)"
        cat "$out/sweep_nuke.txt" 2> /dev/null || echo "(not run in Nuke)"
    } > "$report"
    echo "Report: $report"
    exit 0
fi

effect=${1:-}
section "build (./build.sh)"
./build.sh > build_log.txt 2>&1
status=$?
tail -5 build_log.txt
if [ $status -ne 0 ] || [ ! -x build/millsaph_bench ] || [ ! -d build/MillSaph.ofx.bundle ]; then
    echo "build failed: see build_log.txt"
    exit 1
fi

section "sweep outside any host, 4K (millsaph_bench --sweep)"
rm -f "$out/sweep_nuke.txt"
(cd "$out" && "$root/build/millsaph_bench" --sweep 3840 2160 3 $effect) 2>&1 |
    grep -v '^\[MillSaph\]' | tee "$out/sweep_bench.txt"

cat << EOF

==== next: in Nuke
1. Install this build where Nuke finds OFX plugins, replacing any older copy:

     sudo rm -rf /usr/OFX/Plugins/MillSaph.ofx.bundle
     sudo cp -R "$root/build/MillSaph.ofx.bundle" /usr/OFX/Plugins/

2. Start Nuke as usual (quit it first if it is open), and in its Script Editor
   (Ctrl+Enter):

     MILLSAPH_SWEEP_EFFECT = "$effect"
     exec(open("$root/tools/perf/nuke_sweep.py").read())

   Nuke is busy until the table ends (about an hour for every effect).

3. Then, here:

     tools/perf/sweep_all.sh report
EOF
