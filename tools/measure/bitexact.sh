#!/bin/sh
# Whether a change renders exactly as before: build/'s plugin against a commit's (HEAD by
# default), on the CPU and the GPU, every case of tools/test_cases.txt rendered by test_host,
# compared byte for byte. For refactors (and to see what a change does change).
#
#   tools/measure/bitexact.sh [commit] [cases file]
#
# The commit is built in a git worktree, $MILLSAPH_BITEXACT (default
# ${TMPDIR:-/tmp}/millsaph_bitexact; kept between runs: only what changed is rebuilt). Run
# ./build.sh first for build/. Prints the cases that differ; exit status 1 if any does. A case
# that renders differently from one render to the next (rendered twice after: e.g. GPU sums
# in any order) is said apart, with its largest differences, and not counted.
set -e
cd "$(dirname "$0")/../.."
root=$PWD
commit=$(git rev-parse "${1:-HEAD}") # (resolved here, not in the worktree)
cases=${2:-tools/test_cases.txt}
tree=${MILLSAPH_BITEXACT:-${TMPDIR:-/tmp}/millsaph_bitexact}
. tools/plugin_path.sh

if [ ! -d "$tree" ]; then
    git worktree add --detach "$tree" "$commit"
else
    git -C "$tree" checkout -q --detach "$commit"
fi
echo "building $(git -C "$tree" log --oneline -1) in $tree"
# (with this repository's third-party sources: nothing downloaded again)
(cd "$tree" && MILLSAPH_THIRD_PARTY_DIR=${MILLSAPH_THIRD_PARTY_DIR:-$root/third_party} \
    ./build.sh > build.log 2>&1) || { echo "the build failed: $tree/build.log"; exit 1; }

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# render <plugin> <folder>: every case, on the GPU and the CPU (CUDA hidden)
render() {
    mkdir -p "$2"
    i=0
    grep -v '^ *#' "$cases" | grep -v '^ *$' | while read -r effect args; do
        # (a lens or style file of the repository: @tools/... from its root)
        args=$(echo "$args" | sed "s|=@tools/|=@$root/tools/|g")
        for api in gpu cpu; do
            if [ $api = cpu ]; then hide=CUDA_VISIBLE_DEVICES=; else hide=; fi
            # shellcheck disable=SC2086
            env $hide "$root/build/test_host" "$1" "com.xiaoluo.millsaph.$effect" $args \
                --size 512x384 --save "$2/$i.$api.raw" > "$2/$i.$api.log" 2>&1 || true
        done
        i=$((i + 1))
    done
}
render "$tree/build/MillSaph.ofx.bundle/$plugin_lib" "$out/before"
render "$root/build/MillSaph.ofx.bundle/$plugin_lib" "$out/after"
render "$root/build/MillSaph.ofx.bundle/$plugin_lib" "$out/again"
# the largest difference of two renders
largest() {
    python3 -c "import sys, numpy as n; a, b = (n.fromfile(f, n.float32) for f in sys.argv[1:]); print('%.3g' % abs(a - b).max())" "$1" "$2"
}

n=0
differ=0
failed=0
unstable=0
i=0
grep -v '^ *#' "$cases" | grep -v '^ *$' > "$out/list"
while read -r line; do
    for api in gpu cpu; do
        n=$((n + 1))
        if [ ! -f "$out/before/$i.$api.raw" ] || [ ! -f "$out/after/$i.$api.raw" ]; then
            failed=$((failed + 1))
            echo "FAILED  $api  $line"
        elif ! cmp -s "$out/before/$i.$api.raw" "$out/after/$i.$api.raw"; then
            if cmp -s "$out/after/$i.$api.raw" "$out/again/$i.$api.raw"; then
                differ=$((differ + 1))
                echo "DIFFERS $api  $line"
            else
                unstable=$((unstable + 1))
                echo "UNSTABLE $api  $line (before / after $(largest "$out/before/$i.$api.raw" "$out/after/$i.$api.raw"), after / again $(largest "$out/after/$i.$api.raw" "$out/again/$i.$api.raw"))"
            fi
        fi
    done
    i=$((i + 1))
done < "$out/list"
echo "$n renders: $differ differ, $failed failed (one side or both), $unstable not the same twice"
[ $differ -eq 0 ] && [ $failed -eq 0 ]
