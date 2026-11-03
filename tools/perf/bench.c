// Times every effect on every API built for this machine (the CPU, and Metal or CUDA, ...),
// checks each GPU result against the CPU one, and times the choices the effects
// make that were tuned on another machine (effects/profile.h). Run by tools/perf/collect.sh;
// built with the plugin (build/millsaph_bench).
//
//   millsaph_bench [width height] [renders] [case]   (default 3840 2160, 5; case: a name, or
//                                                    part of the names to run)
//   millsaph_bench --sweep [width height] [renders] [effect]
//   millsaph_bench --transfer [width height]          (CUDA copy speeds: transfer.c)
//   millsaph_bench --dump DIR [width height] [renders] [case]
//
// --dump: also writes each result to DIR/<case>_<api>.raw (floats), to check that a change
// of the code leaves every pixel as it was (cmp the folders before and after).
//
// --sweep: every effect (or the one named, e.g. MS_Distort), each parameter over a series of
// values taken from its slider (CHOICE: every entry; BOOL: both), the others at their
// defaults; EdgeAwareBlur for each Blur Type. Mask parameters are swept with the Mask
// connected, and each optional input is tried once. Then the images: each effect at its
// defaults on each test image (images.c: generated, and the EXR files added to images/),
// and with particular Mask and other input images. Each setting renders as in a host (the
// first API that takes it: the line says which) and on the CPU, compared. Also written to
// millsaph_sweep.csv; the images to images/ (EXR), where nuke_sweep.py reads them.

// clock_gettime: hidden by glibc in strict C11 without it.
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/clock.h"
#include "common/effect.h"
#include "common/half.h"
#include "effects/profile.h"
#include "engine/engine.h"
#include "images.h"
#include "transfer.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#define make_dir(path) mkdir((path), 0755)
#endif

static int width = 3840, height = 2160, renders = 5;

static const MsEffect *find_effect(const char *name) {
    for (int i = 0; i < ms_effect_count; ++i)
        if (strcmp(ms_effects[i]->label, name) == 0)
            return ms_effects[i];
    return NULL;
}

// A case: an effect, its settings ("name=value ..."; WIDTH values as fractions of the width,
// like the hosts) and a change of the profile (NULL: the effects' own).
typedef struct {
    const char *label;
    const char *effect;
    const char *settings;
    void (*tweak)(MsProfile *profile);
} Case;

static void never_short(MsProfile *p) { p->sharp_short_margin = 0; }
static void always_short(MsProfile *p) { p->sharp_short_margin = 100000; }
static void softer_cpu(MsProfile *p) { p->softer_gpu = 0; }
static void plain(MsProfile *p) {
    p->bandpass_staged = 0;
    p->bandpass_prefix = 0;
}
static void no_prefix(MsProfile *p) { p->bandpass_prefix = 0; }
static void rows(MsProfile *p) { p->rows_margin = 0; }
static void blocks(MsProfile *p) { p->rows_margin = 100000; }
static void soft_gpu(MsProfile *p) { p->soft_gpu = 1; }
static void soft_cpu(MsProfile *p) { p->soft_gpu = 0; }

static MsProfile defaults[MS_API_COUNT]; // the effects' own profiles

// Every parameter at its default, then `settings`.
static void set_values(const MsEffect *e, const char *settings, double *values) {
    for (int i = 0; i < e->param_count; ++i)
        values[i] = e->params[i].def;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", settings);
    for (char *t = strtok(copy, " "); t; t = strtok(NULL, " ")) {
        char name[64];
        double v;
        if (sscanf(t, "%63[^=]=%lf", name, &v) != 2)
            continue;
        for (int i = 0; i < e->param_count; ++i)
            if (strcmp(e->params[i].name, name) == 0)
                values[i] = v;
    }
    for (int i = 0; i < e->param_count; ++i) {
        if (e->params[i].type == MS_PARAM_WIDTH)
            values[i] *= width;
        else if (e->params[i].type == MS_PARAM_HEIGHT)
            values[i] *= height;
    }
}

// 1 when `word` is one of the space-separated words of `settings`.
static int has_word(const char *settings, const char *word) {
    const size_t n = strlen(word);
    for (const char *at = strstr(settings, word); at; at = strstr(at + 1, word))
        if ((at == settings || at[-1] == ' ') && (at[n] == ' ' || at[n] == '\0'))
            return 1;
    return 0;
}

// Renders `r` `renders` times on the API set (ms_api_only). Returns the best time after the
// first (ms; the first's with one render), -2 when the effect does not run on that API (it
// declines it, e.g. Soft on the GPU), or -1 on failure (the reason in r->err); `first`: the
// first render's time.
static double time_renders(const MsEffect *e, const MsRender *r, double *first) {
    double best = -1;
    *first = -1;
    for (int i = 0; i < renders; ++i) {
        const double t = ms_clock();
        if (!e->render(r))
            return r->info->declined ? -2 : -1;
        const double ms = ms_clock() - t;
        if (i == 0)
            *first = ms;
        else if (best < 0 || ms < best)
            best = ms;
    }
    return renders > 1 ? best : *first;
}

// Renders the case on `api` `renders` times. Returns the best time after the first (ms), -2
// when the effect does not run on `api` (it declines it, e.g. Soft on the GPU), or -1 on
// failure; `first`: the first render's time. With `half`, the images are given in 16-bit
// floats (as hosts working in half do), the result converted back to floats in `output`.
static double run_case(const Case *c, MsApi api, const float *source, float **blank, float *output,
                       double *first, int half) {
    const MsEffect *e = find_effect(c->effect);
    if (!e) {
        printf("    no effect %s\n", c->effect);
        return -1;
    }
    double values[MS_MAX_PARAMS];
    int connected[MS_MAX_CLIPS] = {0};
    MsInput inputs[MS_MAX_CLIPS];
    set_values(e, c->settings, values);
    // "same": every input connected to the Source's image itself, as a host plugging the
    // Source in everywhere (the effects upload it once).
    // "lens": the Lens connected (to an image of its own).
    // "pageable": CUDA copies from the images as they are, not page-locked (engine.h).
    const int same = has_word(c->settings, "same"), lens = has_word(c->settings, "lens");
    const size_t floats = (size_t)width * height * 4;
    uint16_t *halves = half ? malloc(floats * sizeof(uint16_t)) : NULL;
    uint16_t *result = half ? malloc(floats * sizeof(uint16_t)) : NULL;
    if (half && (!halves || !result)) {
        free(halves);
        free(result);
        return -1;
    }
    for (size_t i = 0; half && i < floats; ++i)
        halves[i] = ms_half_from_float(source[i]);
    const void *image = half ? (const void *)halves : (const void *)source;
    for (int k = 0; k < e->clip_count; ++k) { // blank: zeros, in floats or halves
        connected[k] = k == 0 || same || (lens && strcmp(e->clips[k].clip, "Lens") == 0);
        inputs[k] = (MsInput){k == 0 || same ? image : blank[k], (ptrdiff_t)width * 4};
    }
    char err[256] = "";
    MsRunInfo info;
    MsRender r = {values, connected, inputs,   width,      height,
                  width,  height,    half ? (void *)result : (void *)output,
                  (ptrdiff_t)width * 4, err, sizeof err, half, &info};
    MsProfile profile = defaults[api];
    if (c->tweak)
        c->tweak(&profile);
    ms_profile_set(api, &profile);
    ms_pin_host(!has_word(c->settings, "pageable"));
    ms_api_only(api);
    const double best = time_renders(e, &r, first);
    if (best == -1)
        printf("    render failed: %s\n", err);
    for (size_t i = 0; half && best >= 0 && i < floats; ++i)
        output[i] = ms_float_from_half(result[i]);
    free(halves);
    free(result);
    return best;
}

// ---------------------------------------------------------------- sweep

// Effects swept once per entry of one of their CHOICE parameters (their main mode).
static const struct {
    const char *effect, *param;
} modes[] = {{"MS_EdgeAwareBlur", "blur_type"}};

// The values to try for `p`, in the host's units (WIDTH: fraction of the width): its
// slider's ends, quarter and middle, and its default; CHOICE: every entry; BOOL: both. Returns
// how many (at most 8).
static int series(const MsParam *p, double *v) {
    int n = 0;
    switch (p->type) {
    case MS_PARAM_TEXT:
        return 0;
    case MS_PARAM_CHOICE:
        for (int i = 0; i < p->entry_count && i < 8; ++i)
            v[n++] = i;
        return n;
    case MS_PARAM_BOOL:
        v[0] = 0;
        v[1] = 1;
        return 2;
    default:
        break;
    }
    double lo = p->display_min, hi = p->display_max;
    if (lo == 0 && hi == 0) // no slider range: around the default
        hi = p->def != 0 ? 4 * fabs(p->def) : 1;
    const double candidates[] = {lo, lo + (hi - lo) / 4, p->def, lo + (hi - lo) / 2, hi};
    for (int c = 0; c < 5; ++c) {
        double x = candidates[c];
        if (p->min != 0 || p->max != 0) // hard limits
            x = x < p->min ? p->min : x > p->max ? p->max : x;
        int seen = 0;
        for (int k = 0; k < n; ++k)
            seen |= fabs(v[k] - x) < 1e-12;
        if (!seen)
            v[n++] = x;
    }
    for (int i = 1; i < n; ++i) // ascending
        for (int k = i; k > 0 && v[k] < v[k - 1]; --k) {
            const double t = v[k];
            v[k] = v[k - 1];
            v[k - 1] = t;
        }
    return n;
}

static double in_pixels(const MsParam *p, double v) {
    return p->type == MS_PARAM_WIDTH ? v * width : p->type == MS_PARAM_HEIGHT ? v * height : v;
}

// Renders `renders` times on `api` (MS_API_COUNT: the first that takes it, as in a host).
// Returns the best time after the first (ms), or -1 with the reason in err; `first`: the
// first render's time; `used`: the API it ran on.
static double timed(const MsEffect *e, const double *values, const int *connected,
                    const MsInput *inputs, float *output, MsApi api, double *first, MsApi *used,
                    char *err, size_t err_size) {
    MsRunInfo info;
    MsRender r = {values, connected, inputs,   width, height, width, height, output,
                  (ptrdiff_t)width * 4, err, err_size, 0, &info};
    ms_api_only(api);
    const double best = time_renders(e, &r, first);
    *used = info.api;
    return best < 0 ? -1 : best;
}

typedef struct {
    const MsEffect *e;
    const float *source;
    float *cpu, *gpu;
    FILE *csv;
    const float *images[MS_MAX_CLIPS]; // per input, an image other than the Source (NULL: the Source)
} Sweep;

// One setting: `values` (pixels), the inputs in `connected`; printed with `mode`, `what` and
// `value`. `setting`: the same for tools/perf/nuke_sweep.py ("name=value ...", host units,
// CHOICE as an index, WIDTH / HEIGHT as a fraction followed by w / h; "input=Clip/index" for
// each optional input connected, index: its place among the effect's inputs).
static void sweep_one(const Sweep *s, const double *values, const int *connected, const char *mode,
                      const char *what, const char *value, const char *setting) {
    MsInput inputs[MS_MAX_CLIPS];
    for (int k = 0; k < s->e->clip_count && k < MS_MAX_CLIPS; ++k) { // connected: its image, or the Source
        const float *image = s->images[k] ? s->images[k] : s->source;
        inputs[k] = (MsInput){connected[k] ? image : NULL, (ptrdiff_t)width * 4};
    }
    char err[256] = "", cpu_err[256] = "";
    double first = 0, cpu_first = 0;
    MsApi used = MS_API_COUNT, cpu_used = MS_API_COUNT;
    const double t = timed(s->e, values, connected, inputs, s->gpu, MS_API_COUNT, &first, &used,
                           err, sizeof err);
    const double c = timed(s->e, values, connected, inputs, s->cpu, MS_API_CPU, &cpu_first,
                           &cpu_used, cpu_err, sizeof cpu_err);
    const size_t floats = (size_t)width * height * 4;
    double worst = 0;
    for (size_t k = 0; t >= 0 && c >= 0 && used != MS_API_CPU && k < floats; ++k) {
        const double d = fabs((double)s->gpu[k] - s->cpu[k]);
        worst = d > worst ? d : worst;
    }
    const char *on = t >= 0 ? ms_api_name(used) : "FAILED";
    printf("%-18s %-14s %-18s %-16s %-7s %9.1f %9.1f %9.1f %10.2g%s%s\n", s->e->label, mode, what,
           value, on, t, first, c, worst, t < 0 ? "  " : "", t < 0 ? err : "");
    fflush(stdout);
    if (s->csv)
        fprintf(s->csv, "%s,%s,%s,%s,%s,%.2f,%.2f,%.2f,%.3g,\"%s\",%s\n", s->e->label, mode, what,
                value, on, t, first, c, worst, t < 0 ? err : "", setting);
    fflush(s->csv); // kept if the sweep stops before the end
}

// The modes of `e` (modes[]): their number (1 without), and mode m's values (the others at
// their defaults), name ("-" without) and setting ("blur_type=1", "" without).
static int mode_param(const MsEffect *e) {
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; ++m)
        for (int i = 0; i < e->param_count; ++i)
            if (strcmp(modes[m].effect, e->label) == 0 &&
                strcmp(modes[m].param, e->params[i].name) == 0)
                return i;
    return -1;
}
static int mode_count(const MsEffect *e) {
    const int p = mode_param(e);
    return p >= 0 ? e->params[p].entry_count : 1;
}
static void mode_values(const MsEffect *e, int m, double *base, char mode[64], char setting[64]) {
    const int p = mode_param(e);
    set_values(e, "", base);
    snprintf(mode, 64, "-");
    setting[0] = '\0';
    if (p >= 0) {
        base[p] = m;
        snprintf(mode, 64, "%s", e->params[p].entries[m]);
        snprintf(setting, 64, "%s=%d", e->params[p].name, m);
    }
}

static void sweep_effect(Sweep *s) {
    const MsEffect *e = s->e;
    if (e->param_count > MS_MAX_PARAMS || e->clip_count > MS_MAX_CLIPS)
        return;
    for (int m = 0; m < mode_count(e); ++m) {
        char mode[64], mode_setting[64];
        double base[MS_MAX_PARAMS];
        mode_values(e, m, base, mode, mode_setting);
        int connected[MS_MAX_CLIPS] = {1};
        sweep_one(s, base, connected, mode, "defaults", "", mode_setting);
        // Each optional input connected (the Source image stands in).
        for (int k = 1; k < e->clip_count; ++k) {
            int with[MS_MAX_CLIPS] = {1};
            with[k] = 1;
            char what[64], setting[160];
            snprintf(what, sizeof what, "input %s", e->clips[k].clip);
            snprintf(setting, sizeof setting, "%s input=%s/%d", mode_setting, e->clips[k].clip, k);
            sweep_one(s, base, with, mode, what, "connected", setting);
        }
        for (int i = 0; i < e->param_count; ++i) {
            const MsParam *p = &e->params[i];
            if (i == mode_param(e))
                continue;
            int masked[MS_MAX_CLIPS] = {1}; // mask settings: with the Mask connected
            char inputs[64] = "";
            for (int k = 1; k < e->clip_count; ++k) {
                masked[k] = strstr(p->name, "mask") && e->clips[k].mask;
                if (masked[k])
                    snprintf(inputs, sizeof inputs, " input=%s/%d", e->clips[k].clip, k);
            }
            double v[8];
            const int n = series(p, v);
            for (int j = 0; j < n; ++j) {
                double values[MS_MAX_PARAMS];
                memcpy(values, base, sizeof values);
                values[i] = in_pixels(p, v[j]);
                char value[48];
                if (p->type == MS_PARAM_CHOICE)
                    snprintf(value, sizeof value, "%s", p->entries[(int)v[j]]);
                else if (p->type == MS_PARAM_WIDTH || p->type == MS_PARAM_HEIGHT)
                    snprintf(value, sizeof value, "%g (%.0f px)", v[j], values[i]);
                else
                    snprintf(value, sizeof value, "%g", v[j]);
                char setting[160];
                // WIDTH / HEIGHT: a fraction of the frame width / height ("w" / "h").
                const char *unit = p->type == MS_PARAM_WIDTH    ? "w"
                                   : p->type == MS_PARAM_HEIGHT ? "h"
                                                                : "";
                snprintf(setting, sizeof setting, "%s %s=%.10g%s%s", mode_setting, p->name, v[j],
                         unit, inputs);
                sweep_one(s, values, masked, mode, p->name, value, setting);
            }
        }
    }
}

// ---------------------------------------------------------------- images

#define IMAGES "images" // in the sweep's folder, read by tools/perf/nuke_sweep.py too

// Reads images/<name>.exr into `pixels`; a generated image missing (or of another size) is
// generated and written first. Returns 1 on success, else 0 with the reason printed.
static int load_image(const char *name, const MsTestImage *generated, float *pixels) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/%s.exr", IMAGES, name);
    if (ms_exr_read(path, pixels, width, height, err, sizeof err))
        return 1;
    if (generated) {
        generated->make(pixels, width, height);
        if (ms_exr_write(path, pixels, width, height, generated->full_float) &&
            ms_exr_read(path, pixels, width, height, err, sizeof err)) // as Nuke will read it
            return 1;
    }
    printf("image %s: %s\n", path, generated ? "cannot be written" : err);
    return 0;
}

// The images the sweep's images part renders as the Source: the generated ones (not those
// made for other inputs), then the EXR files added to images/ (real plates).
static int source_images(char names[][64], int most) {
    int n = 0;
    for (int i = 0; i < ms_test_image_count && n < most; ++i)
        if (!ms_test_images[i].input)
            snprintf(names[n++], 64, "%s", ms_test_images[i].name);
#ifndef _WIN32
    DIR *dir = opendir(IMAGES);
    for (struct dirent *f; dir && (f = readdir(dir)) && n < most;) {
        const size_t len = strlen(f->d_name);
        if (len < 5 || len >= 64 || strcmp(f->d_name + len - 4, ".exr") != 0)
            continue;
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(len - 4), f->d_name);
        int generated = 0;
        for (int i = 0; i < ms_test_image_count; ++i)
            generated |= strcmp(ms_test_images[i].name, name) == 0;
        if (!generated)
            snprintf(names[n++], 64, "%s", name);
    }
    if (dir)
        closedir(dir);
#endif
    return n;
}

static const MsTestImage *generated_image(const char *name) {
    for (int i = 0; i < ms_test_image_count; ++i)
        if (strcmp(ms_test_images[i].name, name) == 0)
            return &ms_test_images[i];
    return NULL;
}

// Each effect (each mode) at its defaults on each Source image; then, on the plate, its Mask
// with each Mask image, and each other optional input with the noise image.
static void sweep_images(Sweep *s, const char *only, float *source, float *other) {
    char names[64][64];
    const int count = source_images(names, 64);
    for (int i = 0; i < ms_test_image_count; ++i) // written for Nuke even if unused here
        if (ms_test_images[i].input)
            load_image(ms_test_images[i].name, &ms_test_images[i], other);
    printf("\nThe images (%s/):\n", IMAGES);
    for (int i = 0; i < ms_test_image_count; ++i)
        printf("  %-12s %s\n", ms_test_images[i].name, ms_test_images[i].what);
    for (int i = 0; i < count; ++i)
        if (!generated_image(names[i]))
            printf("  %-12s added by hand\n", names[i]);
    printf("\n");
    for (int i = -1; i < count; ++i) { // -1: the plate with other inputs
        const char *name = i < 0 ? "plate" : names[i];
        if (!load_image(name, generated_image(name), source))
            continue;
        s->source = source;
        for (int f = 0; f < ms_effect_count; ++f) {
            const MsEffect *e = ms_effects[f];
            if (strcmp(e->label, "MS_Test") == 0 || (only && strcmp(e->label, only) != 0) ||
                e->param_count > MS_MAX_PARAMS || e->clip_count > MS_MAX_CLIPS)
                continue;
            s->e = e;
            for (int m = 0; m < mode_count(e); ++m) {
                char mode[64], mode_setting[64];
                double base[MS_MAX_PARAMS];
                mode_values(e, m, base, mode, mode_setting);
                const char *space = mode_setting[0] ? " " : "";
                char setting[256], what[64];
                if (i >= 0) {
                    int connected[MS_MAX_CLIPS] = {1};
                    snprintf(setting, sizeof setting, "%s%simage=%s", mode_setting, space, name);
                    sweep_one(s, base, connected, mode, "image", name, setting);
                    continue;
                }
                for (int k = 1; k < e->clip_count; ++k)
                    for (int j = 0; j < ms_test_image_count; ++j) {
                        const MsTestImage *t = &ms_test_images[j];
                        // The Mask: each Mask image; other inputs: the noise.
                        if (e->clips[k].mask ? !(t->input && strstr(t->name, "mask"))
                                             : strcmp(t->name, "noise") != 0)
                            continue;
                        if (!load_image(t->name, t, other))
                            continue;
                        int connected[MS_MAX_CLIPS] = {1};
                        connected[k] = 1;
                        s->images[k] = other;
                        snprintf(what, sizeof what, "%s image", e->clips[k].clip);
                        snprintf(setting, sizeof setting, "%s%simage=plate input=%s/%d/%s",
                                 mode_setting, space, e->clips[k].clip, k, t->name);
                        sweep_one(s, base, connected, mode, what, t->name, setting);
                        s->images[k] = NULL;
                    }
            }
        }
    }
}

static int sweep(int argc, char **argv) {
    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4)
        renders = atoi(argv[3]);
    const size_t floats = (size_t)width * height * 4;
    float *reference = malloc(floats * sizeof(float));
    Sweep s = {NULL, reference, malloc(floats * sizeof(float)), malloc(floats * sizeof(float)),
               fopen("millsaph_sweep.csv", "w"), {NULL}};
    make_dir(IMAGES);
    // The parameters' image: the reference, as written for Nuke (images/reference.exr).
    if (reference && s.cpu && s.gpu &&
        load_image(ms_test_images[0].name, &ms_test_images[0], reference)) {
        printf("%d x %d, best of %d renders after the first; \"on\": where it ran as in a "
               "host, then the CPU's time and the largest difference with it\n",
               width, height, renders - 1);
        printf("%-18s %-14s %-18s %-16s %-7s %9s %9s %9s %10s\n", "effect", "mode", "parameter",
               "value", "on", "ms", "first ms", "CPU ms", "max diff");
        if (s.csv)
            fprintf(s.csv,
                    "effect,mode,parameter,value,on,ms,first ms,CPU ms,max diff,error,setting\n");
        int swept = 0;
        for (int i = 0; i < ms_effect_count; ++i) {
            s.e = ms_effects[i];
            if (strcmp(s.e->label, "MS_Test") != 0 &&
                (argc < 5 || strcmp(s.e->label, argv[4]) == 0)) {
                sweep_effect(&s);
                ++swept;
            }
        }
        if (!swept) {
            printf("no effect named \"%s\"; the effects:", argc >= 5 ? argv[4] : "");
            for (int i = 0; i < ms_effect_count; ++i)
                printf(" %s", ms_effects[i]->label);
            printf("\n");
        } else {
            float *image = malloc(floats * sizeof(float)), *other = malloc(floats * sizeof(float));
            if (image && other)
                sweep_images(&s, argc >= 5 ? argv[4] : NULL, image, other);
            else
                printf("not enough memory for the images\n");
            free(image);
            free(other);
        }
    } else {
        printf("not enough memory, or no reference image\n");
    }
    if (s.csv)
        fclose(s.csv);
    ms_api_only(MS_API_COUNT);
    ms_engine_shutdown();
    free(reference);
    free(s.cpu);
    free(s.gpu);
    return 0;
}

// Writes `floats` values to DIR/<label>_<api>.raw (spaces as _); `dump`: DIR, or NULL.
static void dump_result(const char *dump, const char *label, const char *api, const float *values,
                        size_t floats) {
    if (!dump)
        return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%s.raw", dump, label, api);
    for (char *p = path + strlen(dump); *p; ++p)
        if (*p == ' ')
            *p = '_';
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(values, sizeof(float), floats, f) != floats)
        printf("    cannot write %s\n", path);
    if (f)
        fclose(f);
}

int main(int argc, char **argv) {
    const char *dump = NULL;
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0) {
        dump = argv[2];
        make_dir(dump);
        argv[2] = argv[0];
        argc -= 2;
        argv += 2;
    }
    if (argc >= 2 && strcmp(argv[1], "--sweep") == 0)
        return sweep(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "--transfer") == 0)
        return ms_transfer_bench(argc >= 4 ? atoi(argv[2]) : 1920, argc >= 4 ? atoi(argv[3]) : 1080);
    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4)
        renders = atoi(argv[3]);
    const size_t floats = (size_t)width * height * 4;
    float *source = malloc(floats * sizeof(float));
    float *cpu = malloc(floats * sizeof(float));
    float *gpu = malloc(floats * sizeof(float));
    float *blank[MS_MAX_CLIPS];
    for (int k = 0; k < MS_MAX_CLIPS; ++k)
        blank[k] = calloc(floats, sizeof(float));
    if (!source || !cpu || !gpu) {
        printf("not enough memory\n");
        return 1;
    }
    ms_test_images[0].make(source, width, height);

    for (int a = 0; a < MS_API_COUNT; ++a)
        defaults[a] = *ms_profile((MsApi)a);
    const Case cases[] = {
        {"BandPass", "MS_BandPass", "", NULL},
        {"BandPass plain", "MS_BandPass", "", plain},
        {"BandPass large", "MS_BandPass", "blur_amount1=0.05 blur_amount2=0.1", NULL},
        {"BandPass large staged", "MS_BandPass", "blur_amount1=0.05 blur_amount2=0.1", no_prefix},
        {"BandPass 960", "MS_BandPass", "blur_amount2=0.25", NULL},
        {"BandPass 960 staged", "MS_BandPass", "blur_amount2=0.25", no_prefix},
        {"EAB Sharp", "MS_EdgeAwareBlur", "blur_type=0", NULL},
        {"EAB Sharp tile256", "MS_EdgeAwareBlur", "blur_type=0", never_short},
        {"EAB Sharp 0.01", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.01", NULL},
        {"EAB Sharp 0.1", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.1", NULL},
        {"EAB Sharp 0.1 tile64", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.1", always_short},
        {"EAB Sharp 0.2", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.2", NULL},
        {"EAB Sharp 0.5", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.5", NULL},
        {"EAB Sharp rows", "MS_EdgeAwareBlur", "blur_type=0", rows},
        {"EAB Sharp blocks", "MS_EdgeAwareBlur", "blur_type=0", blocks},
        {"EAB Sharp 0.01 rows", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.01", rows},
        {"EAB Sharp 0.1 rows", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.1", rows},
        {"EAB Sharp 0.2 rows", "MS_EdgeAwareBlur", "blur_type=0 blur_amount=0.2", rows},
        {"EAB Sharp thresh 1", "MS_EdgeAwareBlur", "blur_type=0 edge_threshold=1", NULL},
        {"EAB Sharp thresh 0.01", "MS_EdgeAwareBlur", "blur_type=0 edge_threshold=0.01", NULL},
        {"EAB Sharp smooth", "MS_EdgeAwareBlur", "blur_type=0 edge_smooth=0.0125", NULL},
        {"EAB Sharp box", "MS_EdgeAwareBlur", "blur_type=0 filter=0", NULL},
        {"EAB Sharp filter smooth", "MS_EdgeAwareBlur", "blur_type=0 filter=3", NULL},
        {"EAB Soft", "MS_EdgeAwareBlur", "blur_type=1", NULL},
        {"EAB Soft gpu", "MS_EdgeAwareBlur", "blur_type=1", soft_gpu},
        {"EAB Soft cpu", "MS_EdgeAwareBlur", "blur_type=1", soft_cpu},
        {"EAB Soft 0.5", "MS_EdgeAwareBlur", "blur_type=1 blur_amount=0.5", NULL},
        {"EAB Soft smooth", "MS_EdgeAwareBlur", "blur_type=1 edge_smooth=0.0125", NULL},
        {"EAB Softer", "MS_EdgeAwareBlur", "blur_type=2", NULL},
        {"EAB Softer cpu", "MS_EdgeAwareBlur", "blur_type=2", softer_cpu},
        {"EAB Softer 0.2", "MS_EdgeAwareBlur", "blur_type=2 blur_amount=0.2", NULL},
        {"EAB Softer 0.2 cpu", "MS_EdgeAwareBlur", "blur_type=2 blur_amount=0.2", softer_cpu},
        {"EAB Softer 0.5", "MS_EdgeAwareBlur", "blur_type=2 blur_amount=0.5", NULL},
        {"EAB Softer rows", "MS_EdgeAwareBlur", "blur_type=2", rows},
        {"EAB Softer blocks", "MS_EdgeAwareBlur", "blur_type=2", blocks},
        {"EAB Softer 0.2 rows", "MS_EdgeAwareBlur", "blur_type=2 blur_amount=0.2", rows},
        {"EAB Softer thresh 1", "MS_EdgeAwareBlur", "blur_type=2 edge_threshold=1", NULL},
        {"BandPass same", "MS_BandPass", "same", NULL},
        {"EAB Sharp same", "MS_EdgeAwareBlur", "blur_type=0 same", NULL},
        {"EAB Soft same", "MS_EdgeAwareBlur", "blur_type=1 same", NULL},
        {"BandPass half", "MS_BandPass", "half", NULL},
        {"EAB Sharp half", "MS_EdgeAwareBlur", "blur_type=0 half", NULL},
        {"EAB Soft half", "MS_EdgeAwareBlur", "blur_type=1 half", NULL},
        {"EAB Softer half", "MS_EdgeAwareBlur", "blur_type=2 half", NULL},
        {"EAB Sharp mask half", "MS_EdgeAwareBlur", "blur_type=0 same half", NULL},
        {"Distort half", "MS_Distort", "half", NULL},
        {"Distort lens half", "MS_Distort", "lens half", NULL},
        {"Distort", "MS_Distort", "", NULL},
        {"Distort same", "MS_Distort", "same", NULL},
        {"Distort lens", "MS_Distort", "lens", NULL},
        {"Distort lens pageable", "MS_Distort", "lens pageable", NULL},
        {"EAB Sharp pageable", "MS_EdgeAwareBlur", "blur_type=0 pageable", NULL},
        {"Distort enable_gpu=0", "MS_Distort", "enable_gpu=0", NULL},
    };

    printf("%d x %d, best of %d renders after the first. GPU APIs built:", width, height,
           renders - 1);
    for (int a = 0; a < MS_API_CPU; ++a)
        if (ms_api_built((MsApi)a))
            printf(" %s", ms_api_name((MsApi)a));
    printf("\n%-24s %-12s %9s %9s %12s %12s\n", "case", "api", "best ms", "first ms", "max diff",
           "mean diff");
    int exact = 0;
    for (size_t i = 0; argc > 4 && i < sizeof cases / sizeof cases[0]; ++i)
        exact |= strcmp(cases[i].label, argv[4]) == 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const Case *c = &cases[i];
        if (argc > 4 && (exact ? strcmp(c->label, argv[4]) != 0 : !strstr(c->label, argv[4])))
            continue; // only the case of this name, else those with this in their name
        // "half": the images in 16-bit floats, compared with the CPU's result in floats
        // (the CPU in half too, after it).
        const int half = has_word(c->settings, "half");
        double first = 0;
        const double t = run_case(c, MS_API_CPU, source, blank, cpu, &first, 0);
        printf("%-24s %-12s %9.1f %9.1f\n", c->label, "CPU", t, first);
        fflush(stdout);
        if (t >= 0)
            dump_result(dump, c->label, "CPU", cpu, floats);
        for (int a = 0; a <= MS_API_CPU; ++a) {
            if (!ms_api_built((MsApi)a) || (a == MS_API_CPU && !half))
                continue;
            // Effects can decline an API (e.g. Soft): then nothing runs on it.
            const double g = run_case(c, (MsApi)a, source, blank, gpu, &first, half);
            if (g == -2) {
                printf("%-24s %-12s (declined: runs on the CPU)\n", c->label,
                       ms_api_name((MsApi)a));
                continue;
            }
            double worst = 0, sum = 0;
            for (size_t k = 0; g >= 0 && k < floats; ++k) {
                const double e = fabs((double)gpu[k] - cpu[k]);
                worst = e > worst ? e : worst;
                sum += e;
            }
            char api_name[32];
            snprintf(api_name, sizeof api_name, "%s%s", ms_api_name((MsApi)a), half ? " half" : "");
            if (g >= 0)
                dump_result(dump, c->label, api_name, gpu, floats);
            printf("%-24s %-12s %9.1f %9.1f %12.2g %12.2g\n", c->label, api_name, g, first, worst,
                   sum / (double)floats);
            fflush(stdout);
        }
    }
    ms_api_only(MS_API_COUNT);
    ms_engine_shutdown();
    for (int k = 0; k < MS_MAX_CLIPS; ++k)
        free(blank[k]);
    free(source);
    free(cpu);
    free(gpu);
    return 0;
}
