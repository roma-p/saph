#!/bin/sh
# Times every effect at 4K outside any host, with tools/test_host.c, on the plugin built by
# ./build.sh. Prints the GPU API the effects really ran on ("[MillSaph] running on CUDA")
# and, per case, the best render time after the first (the first one loads the GPU).
# Distort_CPU: Distort with Enable GPU off.
#
# A new effect gets a case here (and a line in nuke_time.py).
#
#   tools/perf/perf.sh [frames]      (default 6)
set -e
cd "$(dirname "$0")/../.."

frames="${1:-6}"
third_party="${MILLSAPH_THIRD_PARTY_DIR:-$PWD/third_party}"
case "$(uname)" in
Darwin) plugin=build/MillSaph.ofx.bundle/Contents/MacOS/MillSaph.ofx; libs= ;;
*)      plugin=build/MillSaph.ofx.bundle/Contents/Linux-x86-64/MillSaph.ofx; libs=-ldl ;;
esac
if [ ! -f "$plugin" ]; then
    echo "no $plugin: run ./build.sh first"
    exit 1
fi
cc -std=c11 -O2 -I"$third_party/openfx/include" -Isrc tools/test_host.c src/common/half.c $libs \
    -o build/test_host

echo "machine: $(uname -sm), $(nproc 2>/dev/null || sysctl -n hw.ncpu) CPU threads"
command -v nvidia-smi > /dev/null && nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader | sed 's/^/GPU: /'

# label, effect, then parameters (name=value) and unconnected inputs (!Clip)
while read -r label effect args; do
    [ -z "$label" ] && continue
    log=build/perf_last.log
    build/test_host "$plugin" "com.xiaoluo.millsaph.$effect" $args --size 3840x2160 \
        --frames "$frames" > "$log" 2>&1 || { echo "FAILED $label"; tail -5 "$log"; continue; }
    grep -o "\[MillSaph\] .*" "$log" || true # stderr: can land inside a printed line
    awk -v label="$label" '/^render [0-9]+ of/ {
            n++; ms = $NF == "ms" ? $(NF - 1) : $NF
            if (n > 1 && (best == "" || ms + 0 < best + 0)) best = ms
            all = all " " ms
        }
        END { printf "%-22s %8.1f ms per frame (best after the first; all:%s)\n", label, best, all }' "$log"
done <<'CASES'
EdgeAwareBlur_Sharp  EdgeAwareBlur !Mask !Edge_Source blur_type=0
EdgeAwareBlur_Soft   EdgeAwareBlur !Mask !Edge_Source blur_type=1
EdgeAwareBlur_Softer EdgeAwareBlur !Mask !Edge_Source blur_type=2
BandPass             BandPass !Mask
Distort              Distort !Mask !Lens
Distort_CPU          Distort !Mask !Lens enable_gpu=0
CASES
