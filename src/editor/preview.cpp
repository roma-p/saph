// See preview.h.

#include "preview.h"

#include <cmath>
#include <cstring>

#include "common/effect.h"
#include "engine/engine.h"

static const MsEffect *lens_flare() {
    for (int i = 0; i < ms_effect_count; ++i)
        if (std::strcmp(ms_effects[i]->label, "MS_LensFlare") == 0)
            return ms_effects[i];
    return nullptr;
}

// The background: black, grey, or a dusk gradient (bottom row first, as the engine's images).
static void background(int kind, int width, int height, std::vector<float> &px) {
    px.assign((size_t)width * height * 4, 0.0f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float *p = &px[((size_t)y * width + x) * 4];
            const float v = (float)y / (float)(height - 1);
            if (kind == 1)
                p[0] = p[1] = p[2] = 0.18f;
            else if (kind == 2) {
                p[0] = 0.02f + 0.25f * v * v;
                p[1] = 0.02f + 0.12f * v * v;
                p[2] = 0.06f + 0.18f * v;
            }
            p[3] = 1.0f;
        }
}

bool Preview::render(const std::string &text, const PreviewSettings &s, int width, int height,
                     std::vector<uint8_t> &rgba, std::string &err) {
    const MsEffect *e = lens_flare();
    if (!e || e->param_count > MS_MAX_PARAMS) {
        err = "MS_LensFlare is not in this build";
        return false;
    }
    // every setting at its default (in pixels of this frame), then the preview's own
    double values[MS_MAX_PARAMS] = {0};
    const char *texts[MS_MAX_PARAMS] = {nullptr};
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        double v = p->def;
        if (p->type == MS_PARAM_WIDTH || p->type == MS_PARAM_POINT)
            v *= width;
        else if (p->type == MS_PARAM_HEIGHT || p->type == MS_PARAM_POINT_Y)
            v *= height;
        const std::string name = p->name ? p->name : "";
        if (name == "hotspot")
            v = s.hotspot[0] * width;
        else if (name == "hotspot_y")
            v = s.hotspot[1] * height;
        else if (name == "pivot")
            v = s.pivot[0] * width;
        else if (name == "pivot_y")
            v = s.pivot[1] * height;
        else if (name == "scale_widths")
            v = s.scale_widths * width;
        else if (name == "brightness")
            v = s.brightness;
        else if (name == "preset")
            v = 0; // the lens text
        else if (name == "lens_")
            texts[i] = text.c_str();
        values[i] = v;
    }
    background(s.background, width, height, source_);
    output_.assign((size_t)width * height * 4, 0.0f);
    int connected[MS_MAX_CLIPS] = {1};
    MsInput inputs[MS_MAX_CLIPS] = {};
    inputs[0] = MsInput{source_.data(), (ptrdiff_t)width * 4};
    char message[512] = "";
    MsRunInfo info;
    const MsRender r = {values,  connected, inputs,  width,          height, (double)width,
                        (double)height, output_.data(), (ptrdiff_t)width * 4, message,
                        sizeof message, 0,  &info,   texts,          0.0,
                        nullptr,        nullptr};
    if (!e->render(&r)) {
        err = message;
        return false;
    }
    api_ = ms_api_name(info.api);
    // to 8 bits (gamma 2.2 x the preview's), top row first
    const float g = 1.0f / (2.2f * (float)std::fmax(s.gamma, 0.05));
    rgba.resize((size_t)width * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float *p = &output_[((size_t)(height - 1 - y) * width + x) * 4];
            uint8_t *q = &rgba[((size_t)y * width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const float v = p[c] > 0 ? std::pow(std::fmin(p[c], 1.0f), g) : 0.0f;
                q[c] = (uint8_t)(v * 255.0f + 0.5f);
            }
            q[3] = 255;
        }
    return true;
}
