// Starts the MillSaph GPU outside any host and runs one shader pass, to tell apart
// "the GPU / driver does not work" from "it does not work inside Nuke / Flame".
//
//   ./tools/gpu_check.sh builds and runs it (macOS or Linux).

#include <stdio.h>

#include "gpu.h"

static const char* shader =
    "#version 410 core\n"
    "uniform sampler2D source;\n"
    "uniform float gain;\n"
    "out vec4 color;\n"
    "void main() { color = texelFetch(source, ivec2(gl_FragCoord.xy), 0) * gain; }\n";

int main(void) {
    float input[4 * 4] = {0.1f, 0.2f, 0.3f, 0.4f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    float output[4 * 4] = {0};
    const MsUniform gain = {"gain", 2.0f};
    char err[2048];

    printf("GPU: %s\n", ms_gpu_info());
    int ok = ms_gpu_run(shader, input, 2, 2, &gain, 1, output, err, sizeof err);
    if (ok)
        for (int i = 0; i < 16; ++i) ok = ok && output[i] == 2.0f * input[i];
    printf("%s\n", ok ? "OK: shader ran and the result is correct" : err);
    ms_gpu_shutdown();
    return ok ? 0 : 1;
}
