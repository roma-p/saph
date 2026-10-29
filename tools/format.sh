#!/bin/sh
# Formats every C file with clang-format (style in .clang-format).
# `tools/format.sh --check` only lists the files that are not formatted.
set -e
cd "$(dirname "$0")/.."

format=$(command -v clang-format || echo /opt/homebrew/opt/llvm/bin/clang-format)
[ -x "$format" ] || { echo "clang-format not found (macOS: brew install llvm)"; exit 1; }

if [ "$1" = --check ]; then
    "$format" --dry-run --Werror src/*/*.c src/*/*.h tools/*.c
else
    "$format" -i src/*/*.c src/*/*.h tools/*.c
fi
