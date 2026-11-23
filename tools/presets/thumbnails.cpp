// millsaph_thumbnails: each preset's thumbnail (the preset window's), rendered by its effect
// on MillSaph's own test picture, as PNG.
//
//   millsaph_thumbnails <presets folder> <out folder> [effect folder ...] [--size WxH]
//
// For each effect folder (presets/<folder>, MsEffect.presets; all of them by default): copies
// each preset file into <out folder>/<folder>/ and renders <name>.png beside it. The preset
// applied as the plugin applies it (common/preset.c): every parameter at its default, then
// the preset's. Lens flares on black; the other effects on the test picture.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/effect.h"
#include "common/preset.h"
#include "engine/engine.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

int W = 384, H = 216;

// MsHost on a values table: values as the hosts hold them (WIDTH: fractions; POINT: pixels).
struct Table {
    const MsEffect *e;
    std::vector<double> values;
    std::vector<std::string> texts;
};

int index_of(const MsEffect *e, const char *name) {
    for (int i = 0; i < e->param_count; ++i)
        if (e->params[i].name && !std::strcmp(e->params[i].name, name))
            return i;
    return -1;
}

const char *get_text(MsHost *h, const char *name) {
    Table *t = (Table *)h->data;
    const int i = index_of(t->e, name);
    return i >= 0 ? t->texts[i].c_str() : nullptr;
}

int set_text(MsHost *h, const char *name, const char *text) {
    Table *t = (Table *)h->data;
    const int i = index_of(t->e, name);
    if (i >= 0)
        t->texts[i] = text;
    return i >= 0;
}

double get_value(MsHost *h, const char *name) {
    Table *t = (Table *)h->data;
    const int i = index_of(t->e, name);
    return i >= 0 ? t->values[i] : 0.0;
}

int set_value(MsHost *h, const char *name, double v) {
    Table *t = (Table *)h->data;
    const int i = index_of(t->e, name);
    if (i >= 0)
        t->values[i] = v;
    return i >= 0;
}

int frame_size(MsHost *, double size[2]) {
    size[0] = W, size[1] = H;
    return 1;
}

double fract(double x) { return x - std::floor(x); }
double hash(double x, double y) { return fract(std::sin(x * 12.9898 + y * 78.233) * 43758.5453); }

// MillSaph's test picture, linear RGBA, bottom row first: a sky, hills, coloured shapes, a
// striped panel, small bright lights. Only the sun and the lights above 0.7: what glows and
// glints comes from them.
void picture(std::vector<float> &px) {
    px.assign((size_t)W * H * 4, 1.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double u = (x + 0.5) / W, v = (y + 0.5) / H; // v up
            double r, g, b;
            const double hill = 0.38 + 0.07 * std::sin(u * 7.0) + 0.04 * std::sin(u * 17.0 + 1.0);
            if (v > hill) { // sky: blue to warm at the horizon
                const double t = (v - hill) / (1.0 - hill);
                r = 0.55 + (0.12 - 0.55) * t, g = 0.42 + (0.25 - 0.42) * t, b = 0.35 + (0.55 - 0.35) * t;
            } else { // ground: green with a grain
                const double n = hash(std::floor(u * 160), std::floor(v * 90));
                r = 0.07 + 0.05 * n, g = 0.16 + 0.08 * n, b = 0.06 + 0.03 * n;
            }
            // a sun disc
            const double ds = std::hypot((u - 0.72) * 16.0 / 9.0, v - 0.70);
            if (ds < 0.07)
                r = 4.0, g = 3.2, b = 2.2;
            // a red ball, a blue box, a yellow triangle
            if (std::hypot((u - 0.22) * 16.0 / 9.0, v - 0.30) < 0.12)
                r = 0.75, g = 0.08, b = 0.06;
            if (u > 0.40 && u < 0.55 && v > 0.12 && v < 0.36)
                r = 0.08, g = 0.22, b = 0.70;
            if (v > 0.10 && v < 0.34 && std::fabs(u - 0.86) < (0.34 - v) * 0.5)
                r = 0.70, g = 0.56, b = 0.08;
            // a striped panel
            if (u > 0.05 && u < 0.17 && v > 0.55 && v < 0.85) {
                const double s = std::sin(u * 220.0) > 0 ? 0.62 : 0.06;
                r = g = b = s;
            }
            // small bright lights along a line
            for (int k = 0; k < 7; ++k) {
                const double lx = 0.30 + k * 0.05, ly = 0.50 + 0.02 * (k % 2);
                if (std::hypot((u - lx) * W, (v - ly) * H) < 1.6)
                    r = 6.0, g = 5.0, b = 3.0;
            }
            float *p = &px[((size_t)y * W + x) * 4];
            p[0] = (float)r, p[1] = (float)g, p[2] = (float)b, p[3] = 1.0f;
        }
}

std::string read_file(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Renders `text` (a preset) with `e` into an 8-bit RGB PNG at `out`.
bool render(const MsEffect *e, const std::string &text, const std::vector<float> &source,
            const fs::path &out, std::string &err) {
    Table t{e, std::vector<double>(e->param_count, 0.0), std::vector<std::string>(e->param_count)};
    MsHost host = {get_text, set_text, get_value, set_value, &t, frame_size};
    char message[512] = "";
    if (!ms_apply_preset(e, &host, text.c_str(), text.size(), message, sizeof message)) {
        err = message;
        return false;
    }
    std::vector<double> values(MS_MAX_PARAMS, 0.0);
    std::vector<const char *> texts(MS_MAX_PARAMS, nullptr);
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        values[i] = t.values[i] * (p->type == MS_PARAM_WIDTH ? W : p->type == MS_PARAM_HEIGHT ? H : 1);
        if (p->type == MS_PARAM_STRING)
            texts[i] = t.texts[i].c_str();
    }
    int connected[MS_MAX_CLIPS] = {0};
    MsInput inputs[MS_MAX_CLIPS] = {};
    for (int c = 0; c < e->clip_count; ++c) // the picture in the Source and required inputs
        if (c == 0 || (!e->clips[c].optional && !e->clips[c].mask)) {
            connected[c] = 1;
            inputs[c] = MsInput{source.data(), (ptrdiff_t)W * 4};
        }
    std::vector<float> output((size_t)W * H * 4, 0.0f);
    MsRunInfo info;
    MsRender r{}; // (by field: C++17 has no designated initialisers)
    r.values = values.data();
    r.connected = connected;
    r.inputs = inputs;
    r.width = W;
    r.height = H;
    r.frame_width = W;
    r.frame_height = H;
    r.output = output.data();
    r.output_row = (ptrdiff_t)W * 4;
    r.err = message;
    r.err_size = sizeof message;
    r.info = &info;
    r.texts = texts.data();
    r.time = 1.0;
    if (!e->render(&r)) {
        err = message;
        return false;
    }
    std::vector<unsigned char> rgb((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float *p = &output[((size_t)(H - 1 - y) * W + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const float v = p[c] > 0 ? std::pow(std::fmin(p[c], 1.0f), 1.0f / 2.2f) : 0.0f;
                rgb[((size_t)y * W + x) * 3 + c] = (unsigned char)(v * 255.0f + 0.5f);
            }
        }
    return stbi_write_png(out.string().c_str(), W, H, 3, rgb.data(), W * 3) != 0;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args, only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &W, &H);
        else
            args.push_back(argv[i]);
    }
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: millsaph_thumbnails <presets folder> <out folder> "
                             "[effect folder ...] [--size WxH]\n");
        return 2;
    }
    only.assign(args.begin() + 2, args.end());
    std::vector<float> photo, black((size_t)W * H * 4, 0.0f);
    picture(photo);
    for (size_t i = 3; i < black.size(); i += 4)
        black[i] = 1.0f;
    int failed = 0, done = 0;
    for (int k = 0; k < ms_effect_count; ++k) {
        const MsEffect *e = ms_effects[k];
        if (!e->presets || (!only.empty() && std::find(only.begin(), only.end(), e->presets) ==
                                                 only.end()))
            continue;
        const fs::path in = fs::path(args[0]) / e->presets, out = fs::path(args[1]) / e->presets;
        std::error_code ec;
        if (!fs::is_directory(in, ec))
            continue;
        fs::create_directories(out, ec);
        const bool flare = !std::strcmp(e->presets, "lensflare");
        for (const auto &entry : fs::directory_iterator(in, ec)) {
            if (entry.path().extension() != ".json")
                continue;
            const std::string text = read_file(entry.path());
            fs::copy_file(entry.path(), out / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            std::string err;
            fs::path png = out / entry.path().filename();
            png.replace_extension(".png");
            if (render(e, text, flare ? black : photo, png, err))
                ++done;
            else {
                ++failed;
                std::fprintf(stderr, "%s %s: %s\n", e->label,
                             entry.path().filename().string().c_str(), err.c_str());
            }
        }
    }
    std::printf("millsaph_thumbnails: %d thumbnails, %d failed\n", done, failed);
    ms_effects_release();
    ms_engine_shutdown();
    return failed ? 1 : 0;
}
