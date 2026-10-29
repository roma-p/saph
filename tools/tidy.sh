#!/bin/sh
# Static checks with clang-tidy (checks listed in .clang-tidy): likely bugs, found
# without running the code. Needs build/compile_commands.json: run ./build.sh first.
set -e
cd "$(dirname "$0")/.."

tidy=$(command -v clang-tidy || echo /opt/homebrew/opt/llvm/bin/clang-tidy)
[ -x "$tidy" ] || { echo "clang-tidy not found (macOS: brew install llvm)"; exit 1; }
[ -f build/compile_commands.json ] || ./build.sh

# The platform file of the other system is not in the build.
for file in src/*/*.c; do
    case "$file" in src/gpu/gl_linux.c) [ "$(uname)" = Darwin ] && continue ;; src/gpu/gl_mac.c) [ "$(uname)" = Darwin ] || continue ;; esac
    "$tidy" -p build --quiet "$file"
done
