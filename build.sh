#!/bin/sh
set -e
cd "$(dirname "$0")"

# build env var
# TODO move this to rez pacakge.py
export MILLSAPH_VERSION="${MILLSAPH_VERSION:-0.1.0}"
export MILLSAPH_THIRD_PARTY_DIR="${MILLSAPH_THIRD_PARTY_DIR:-$PWD/third_party}"
export MILLSAPH_OFX_VERSION="${MILLSAPH_OFX_VERSION:-OFX_Release_1.5.1}"

cmake -S . -B build
cmake --build build --config Release
