#!/bin/sh
# Static checks with clang-tidy (checks listed in .clang-tidy): likely bugs, found
# without running the code. Needs this build's compile_commands.json: run ./build.sh first.
set -e
cd "$(dirname "$0")/.."
. tools/plugin_path.sh

tidy=$(command -v clang-tidy || echo /opt/homebrew/opt/llvm/bin/clang-tidy)
[ -x "$tidy" ] || { echo "clang-tidy not found (macOS: brew install llvm)"; exit 1; }
[ -f $build_dir/cmake/compile_commands.json ] || ./build.sh

# Only the files this system builds: the After Effects plugin (Windows) is not in the
# build.
for file in src/*/*.c; do
    case "$file" in
    src/ae/*) continue ;;
    esac
    "$tidy" -p $build_dir/cmake --quiet "$file"
done
