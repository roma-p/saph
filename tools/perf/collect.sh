#!/bin/sh
# Collects everything about this machine and MillSaph's speed on it into one report file,
# to send back: the machine (OS, CPU, memory, GPU, drivers, CUDA / Vulkan libraries), the
# build, every effect on every API (tools/perf/bench.c, with the GPU results checked against
# the CPU), the OFX test host's timings (perf.sh), Nuke's timing if nuke_time.py ran, and
# MillSaph's log (~/millsaph.log: renders in Flame or Nuke, with where their time went).
#
#   tools/perf/collect.sh            (builds first; takes a few minutes)
cd "$(dirname "$0")/../.."

report="millsaph_report_$(hostname 2>/dev/null || echo host)_$(date +%Y%m%d_%H%M).txt"
section() { printf '\n==================== %s\n' "$1"; }
run() { # a command, if it exists
    command -v "$1" > /dev/null 2>&1 || { echo "($1: not found)"; return; }
    "$@" 2>&1
}

{
    section "date, source"
    date
    pwd
    run git log -1 --format='%H %s'
    run git status --short

    section "machine"
    run uname -a
    [ -f /etc/os-release ] && cat /etc/os-release
    run sw_vers
    run lscpu
    run sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize
    run free -g
    run nproc
    run ldd --version | head -1

    section "GPU"
    run nvidia-smi
    run nvidia-smi -q | head -120
    run ldconfig -p | grep -iE 'libcuda|libnvidia-ptxjitcompiler|libvulkan'
    run vulkaninfo --summary
    run system_profiler SPDisplaysDataType

    section "tools"
    run cc --version
    run cmake --version
    [ -n "$MILLSAPH_THIRD_PARTY_DIR" ] && echo "MILLSAPH_THIRD_PARTY_DIR=$MILLSAPH_THIRD_PARTY_DIR"

    section "build (./build.sh, last 150 lines)"
    ./build.sh > build_log.txt 2>&1
    echo "exit status: $?"
    tail -150 build_log.txt

    section "bench: every effect on every API, 4K (build/millsaph_bench)"
    if [ -x build/millsaph_bench ]; then
        build/millsaph_bench 3840 2160 5
        section "bench, HD"
        build/millsaph_bench 1920 1080 5
    else
        echo "(not built)"
    fi

    section "sweep: each parameter of each effect over its range, 4K (millsaph_bench --sweep)"
    if [ -x build/millsaph_bench ]; then
        root=$(pwd)
        mkdir -p "$HOME/millsaph_perf"
        (cd "$HOME/millsaph_perf" && "$root/build/millsaph_bench" --sweep 3840 2160 3)
        echo "(also in ~/millsaph_perf/millsaph_sweep.csv, for tools/perf/nuke_sweep.py)"
    fi

    section "Nuke sweep (tools/perf/nuke_sweep.py, if it ran: ~/millsaph_perf/sweep_nuke.txt)"
    if [ -f "$HOME/millsaph_perf/sweep_nuke.txt" ]; then
        cat "$HOME/millsaph_perf/sweep_nuke.txt"
    else
        echo "(no Nuke sweep)"
    fi

    section "perf.sh: through the OFX test host, 4K"
    tools/perf/perf.sh 6

    section "Nuke (tools/perf/nuke_time.py, if it ran: ~/millsaph_perf/timing.txt)"
    if [ -f "$HOME/millsaph_perf/timing.txt" ]; then
        ls -la "$HOME/millsaph_perf/timing.txt"
        cat "$HOME/millsaph_perf/timing.txt"
    else
        echo "(no Nuke timing)"
    fi

    section "MillSaph's log (~/millsaph.log, last 400 lines)"
    if [ -f "$HOME/millsaph.log" ]; then
        tail -400 "$HOME/millsaph.log"
    else
        echo "(no log: the plugin has not run in a host here)"
    fi
} > "$report" 2>&1

echo "Report written: $PWD/$report"
echo "Send that file back (it is plain text)."
