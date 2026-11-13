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

# the cases: tools/test_cases.txt
failed=0
while read -r effect args; do
    case "$effect" in '' | '#'*) continue ;; esac
    if build-sanitize/test_host "$plugin" "com.xiaoluo.millsaph.$effect" $args > build-sanitize/last.log 2>&1; then
        echo "ok      $effect $args"
    else
        echo "FAILED  $effect $args"
        cat build-sanitize/last.log
        failed=1
    fi
done < tools/test_cases.txt
[ $failed = 0 ] && echo "all clean" || exit 1
