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
Mosaic
Mosaic pixel_frequency=17 pixel_rel_height=1.37 pixel_shift_x=0.003 pixel_shift_y=-0.01 smooth_colors=0.01 crop_left=0.1 crop_top=0.2 blur_mask=0.02 mask_use=1 invert_mask=1 mix_with_source=0.3
Mosaic !Mask smooth_colors=0.2 crop_left=0.9 crop_right=0.5 pixel_frequency=4
Mosaic !Mask pixel_frequency=200 pixel_rel_height=0.1 crop_bottom=3 crop_top=3
Mosaic smooth_colors=0.01 --half --pad 3
Glow
Glow !Mask glow_width=0.013 width_x=0.3 width_y=2.7 subpixel=1 threshold=0.2 threshold_add_color=0.1,0,0.3 color=1,0.5,0.25 combine=4 affect_alpha=0.5 glow_under_source=0.3 source_opacity=0.7 glow_from_alpha=0.4
Glow invert_mask=1 edge_mode=0 combine=0 subpixel=0 glow_width=0.5
Glow !Mask show=1 threshold=1.2
Glow --half --pad 3
EdgeBlur
EdgeBlur !Matte blur_width=0.013 edge_width=0.021 edge_strength=2 edge_threshold=0.1 subpixel=1
EdgeBlur matte_use=1 subpixel=0 blur_width=0.5 edge_width=0.4
EdgeBlur !Matte show=1
EdgeBlur --half --pad 3
RackDefocus
RackDefocus !Mask defocus_width=0.03 shape=5 roundness=-0.5 bokeh=-0.5 color_fringing=0.3 gauss_blur=0.01 use_gamma=2 boost_highlights=1 hilight_threshold=0.5 mode=1 brightness=1.5 offset_darks=0.2 mix_with_source=0.3 edge_mode=0
RackDefocus mask_use=1 invert_mask=1 blur_mask=0.02 edge_mode=1 rel_height=2
RackDefocus !Mask defocus_width=0 gauss_blur=0.01
RackDefocus --half --pad 3
Glint
Glint !Mask size=0.05 size_x=0.5 size_diag2=1.3 size_red=1 brightness_y=0.5 brightness_diag1=2 scale_colors=1,0.5,0.2 threshold=0.3 threshold_add_color=0.1,0,0.2 threshold_blur=0.004 blur_glint=0.003 hue_shift=0.3 affect_alpha=0.5 glint_from_alpha=0.3 glint_under_source=0.4 source_opacity=0.7 swap_diagonals=1
Glint mask_type=1 invert_mask=1 threshold=0
Glint mask_type=2 threshold=1.2 size=0.6
Glint --half --pad 3
CASES
[ $failed = 0 ] && echo "all clean" || exit 1
