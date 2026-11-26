#!/bin/sh
set -e
cd "$(dirname "$0")"

# build env var
# TODO move this to rez package.py
export MILLSAPH_VERSION="${MILLSAPH_VERSION:-0.1.0}"
export MILLSAPH_THIRD_PARTY_DIR="${MILLSAPH_THIRD_PARTY_DIR:-$PWD/third_party}"
export MILLSAPH_OFX_VERSION="${MILLSAPH_OFX_VERSION:-OFX_Release_1.5.1}"

# build/mac, build/lin: CMake's work in cmake/, the plugins in release/, the programs to run
# here in bin/ (tools/plugin_path.sh)
. tools/plugin_path.sh
cmake -S . -B "$build_dir/cmake"
cmake --build "$build_dir/cmake" --config Release
# (for clangd, .clangd: this system's compile commands)
ln -sf "${build_dir#build/}/cmake/compile_commands.json" build/compile_commands.json
