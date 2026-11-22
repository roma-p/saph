#!/bin/sh
# Builds MillSaph with AddressSanitizer and UndefinedBehaviorSanitizer (in
# build-sanitize/) and renders every effect with test_host, in several settings.
# A memory error or undefined behaviour stops the render with a report naming the line.
set -e
cd "$(dirname "$0")/.."

export MILLSAPH_VERSION="${MILLSAPH_VERSION:-0.1.0}"
export MILLSAPH_THIRD_PARTY_DIR="${MILLSAPH_THIRD_PARTY_DIR:-$PWD/third_party}"
export MILLSAPH_OFX_VERSION="${MILLSAPH_OFX_VERSION:-OFX_Release_1.5.1}"

cmake -S . -B build-sanitize -DMILLSAPH_SANITIZE=ON -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build build-sanitize

# (build-sanitize/test_host: CMake's, built with the sanitizers too)
. tools/plugin_path.sh
plugin=build-sanitize/MillSaph.ofx.bundle/$plugin_lib

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
