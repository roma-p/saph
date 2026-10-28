#!/bin/sh
# Builds and runs tools/gpu_check.c: starts the MillSaph GPU outside any host
# (Nuke, Flame) and runs one shader. Run it on the machine where the host runs.
set -e
cd "$(dirname "$0")/.."

# The OpenGL / EGL headers are downloaded by the first ./build.sh.
[ -d third_party/khronos ] || ./build.sh

mkdir -p build
case "$(uname)" in
Darwin)
    cc -std=c11 -DGL_SILENCE_DEPRECATION -Isrc -Ithird_party/khronos \
        tools/gpu_check.c src/gpu.c src/gpu_mac.c -framework OpenGL -o build/gpu_check ;;
*)
    cc -std=c11 -Isrc -Ithird_party/khronos \
        tools/gpu_check.c src/gpu.c src/gpu_linux.c -ldl -lpthread -o build/gpu_check ;;
esac

echo "host: $(hostname)  DISPLAY=${DISPLAY:-none}"
./build/gpu_check
