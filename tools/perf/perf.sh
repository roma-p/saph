#!/bin/sh
# Times every effect at 4K outside any host, with build/test_host, on the plugin built by
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
. tools/plugin_path.sh
plugin=build/MillSaph.ofx.bundle/$plugin_lib
if [ ! -f "$plugin" ] || [ ! -x build/test_host ]; then
    echo "no $plugin or build/test_host: run ./build.sh first"
    exit 1
fi

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
Mosaic               Mosaic !Mask
Mosaic_Smooth        Mosaic !Mask smooth_colors=0.005
Glow                 Glow !Mask !Back
EdgeBlur             EdgeBlur !Matte
RackDefocus          RackDefocus !Mask
Glint                Glint !Mask
Glare                Glare !Mask
LensFlare            LensFlare !Occlusion
LensFlare_Atmosphere LensFlare !Occlusion atmosphere_amp=1
LensFlare_Glow       LensFlare diffraction_glow=1
JpegDamage           JpegDamage !Mask
JpegDamage_Freq      JpegDamage !Mask quality=0.3 x_freq_scale=3 high_freq_scale=0
PixelSort            PixelSort !Mask
PixelSort_Radial     PixelSort !Mask Mode=1
PixelSort_Circular   PixelSort !Mask Mode=2
FlickerMatch         FlickerMatch
FlickerMatchColor    FlickerMatchColor
Flicker              Flicker
TVDamage             TVDamage !Mask
TVDamage_Orthicon    TVDamage !Mask orthicon=1
CASES
