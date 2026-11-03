#!/bin/sh
# Builds MillSaph with AddressSanitizer and UndefinedBehaviorSanitizer (in
# build-sanitize/) and renders every effect with tools/test_host.c, in several settings.
# A memory error or undefined behaviour stops the render with a report naming the line.
set -e
cd "$(dirname "$0")/.."

export MILLSAPH_VERSION="${MILLSAPH_VERSION:-0.1.0}"
export MILLSAPH_THIRD_PARTY_DIR="${MILLSAPH_THIRD_PARTY_DIR:-$PWD/third_party}"
export MILLSAPH_OFX_VERSION="${MILLSAPH_OFX_VERSION:-OFX_Release_1.5.1}"

cmake -S . -B build-sanitize -DMILLSAPH_SANITIZE=ON -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build build-sanitize

flags="-std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$MILLSAPH_THIRD_PARTY_DIR/openfx/include"
case "$(uname)" in
Darwin) plugin=build-sanitize/MillSaph.ofx.bundle/Contents/MacOS/MillSaph.ofx; libs= ;;
*)      plugin=build-sanitize/MillSaph.ofx.bundle/Contents/Linux-x86-64/MillSaph.ofx; libs=-ldl ;;
esac
# The compiler CMake found: its sanitizer libraries (another one, e.g. the system's cc next to
# a newer toolchain's, may not have them).
cc=$(sed -n 's/^CMAKE_C_COMPILER:[A-Z]*=//p' build-sanitize/CMakeCache.txt)
"${cc:-cc}" $flags -Isrc tools/test_host.c src/common/half.c $libs -o build-sanitize/test_host

# effect, then parameters (name=value) and unconnected inputs (!Clip)
failed=0
while read -r effect args; do
    [ -z "$effect" ] && continue
    if build-sanitize/test_host "$plugin" "com.xiaoluo.millsaph.$effect" $args > build-sanitize/last.log 2>&1; then
        echo "ok      $effect $args"
    else
        echo "FAILED  $effect $args"
        cat build-sanitize/last.log
        failed=1
    fi
done <<'CASES'
Test gain=0.5
BandPass
BandPass blur_amount1=0.05 blur_amount2=0.1 blur_rel_x=2 saturation=0.3 mask_use=1 invert_mask=1 blur_mask=0.05
BandPass !Mask
EdgeAwareBlur
EdgeAwareBlur !Mask !Edge_Source blur_amount=0.2 edge_threshold=0.3
EdgeAwareBlur !Mask blur_amount=0.2 blur_type=1 filter=0 edge_smooth=0.02 use_alpha_edges=1 blur_rel_y=0.5
EdgeAwareBlur !Edge_Source blur_amount=0.15 edge_threshold=1 blur_type=2 filter=3 invert_mask=1
EdgeAwareBlur !Mask !Edge_Source blur_amount=0.13 blur_type=0 subpixel=0
Distort
Distort enable_gpu=0
Distort amount=1.5 rotate_warp_dir=30 wrap_x=1 wrap_y=0 apply_mask=1 crop_left=0.1 crop_top=0.2 blur_mask=0.05 mask_lens=1
Distort amount=1.5 rotate_warp_dir=30 wrap_x=1 wrap_y=0 apply_mask=1 crop_left=0.1 crop_top=0.2 blur_mask=0.05 mask_lens=1 enable_gpu=0
Distort amount=-0.7 fine=1 filter=0 amount_rel_x=0.3 invert_mask=1 mask_use=1 crop_bottom=0.3 crop_right=0.25 blur_lens=0.1 enable_gpu=0
Distort !Lens !Mask amount=3 enable_gpu=0
EdgeAwareBlur !Mask edge_smooth=3
Distort blur_lens=3 blur_mask=3 enable_gpu=0
BandPass blur_amount1=3 blur_amount2=4 blur_mask=3
BandPass --half
EdgeAwareBlur --half --pad 3
EdgeAwareBlur blur_type=1 --half
Distort blur_mask=0.05 --half
Distort enable_gpu=0 --half --pad 2
CASES
[ $failed = 0 ] && echo "all clean" || exit 1
