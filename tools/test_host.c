// A minimal OFX host, to run the plugin outside Nuke or Flame (tools/sanitize.sh runs it
// on a sanitizer build). It loads the plugin, then for one effect: describes it, creates
// an instance and renders one frame of a test pattern with every input connected.
//
//   test_host <plugin.ofx> <effect id> [name=value ...] [!Clip ...] [--size WxH] [--frames N]
//             [--save file] [--pad N]
//
//   name=value   sets a parameter (choices and checkboxes by number)
//   !Clip        leaves that input unconnected, e.g. !Mask
//   --size WxH   renders W x H pixels (default 97 x 61) of a photo-like image (gradients,
//                a disc, a box, a little noise) instead of the test pattern
//   --frames N   renders N times and prints the time of each render (tools/perf/)
//   --save file  writes the Output (raw RGBA floats, bottom row first), to compare renders
//   --pad N      images N pixels larger than the render window on each side (rows longer
//                than the window, as hosts can give)
//
// Prints what the effect declares and the result of each action; exits with 1 if an
// action fails. Only the host functions MillSaph uses are implemented.

// clock_gettime: hidden by glibc under -std=c11 without it.
#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

// Odd sizes by default, to catch mistakes with odd rows and columns.
static int width = 97, height = 61, photo = 0;
static int pad = 0; // --pad: images cover the render window and `pad` more pixels per side

// ---------------------------------------------------------------- property sets

#define MAX_PROPS 64

typedef struct {
    char name[96];
    int index;
    double number; // ints and doubles
    char text[256];
    void *pointer;
} Prop;

typedef struct {
    const char *owner; // printed with each value set
    int print;
    Prop props[MAX_PROPS];
    int count;
} PropSet;

static Prop *find(OfxPropertySetHandle handle, const char *name, int index, int create) {
    PropSet *set = (PropSet *)handle;
    for (int i = 0; i < set->count; ++i)
        if (strcmp(set->props[i].name, name) == 0 && set->props[i].index == index)
            return &set->props[i];
    if (!create || set->count == MAX_PROPS)
        return NULL;
    Prop *p = &set->props[set->count++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->index = index;
    return p;
}

static OfxStatus set_pointer(OfxPropertySetHandle h, const char *name, int index, void *value) {
    find(h, name, index, 1)->pointer = value;
    return kOfxStatOK;
}

static OfxStatus set_string(OfxPropertySetHandle h, const char *name, int index,
                            const char *value) {
    snprintf(find(h, name, index, 1)->text, 256, "%s", value ? value : "");
    if (((PropSet *)h)->print)
        printf("    %s %s[%d] = \"%s\"\n", ((PropSet *)h)->owner, name, index, value);
    return kOfxStatOK;
}

static OfxStatus set_double(OfxPropertySetHandle h, const char *name, int index, double value) {
    find(h, name, index, 1)->number = value;
    if (((PropSet *)h)->print)
        printf("    %s %s[%d] = %g\n", ((PropSet *)h)->owner, name, index, value);
    return kOfxStatOK;
}

static OfxStatus set_int(OfxPropertySetHandle h, const char *name, int index, int value) {
    return set_double(h, name, index, value);
}

static OfxStatus get_pointer(OfxPropertySetHandle h, const char *name, int index, void **value) {
    const Prop *p = find(h, name, index, 0);
    if (!p)
        return kOfxStatErrUnknown;
    *value = p->pointer;
    return kOfxStatOK;
}

static OfxStatus get_string(OfxPropertySetHandle h, const char *name, int index, char **value) {
    Prop *p = find(h, name, index, 0);
    if (!p)
        return kOfxStatErrUnknown;
    *value = p->text;
    return kOfxStatOK;
}

static OfxStatus get_double(OfxPropertySetHandle h, const char *name, int index, double *value) {
    const Prop *p = find(h, name, index, 0);
    if (!p)
        return kOfxStatErrUnknown;
    *value = p->number;
    return kOfxStatOK;
}

static OfxStatus get_int(OfxPropertySetHandle h, const char *name, int index, int *value) {
    const Prop *p = find(h, name, index, 0);
    if (!p)
        return kOfxStatErrUnknown;
    *value = (int)p->number;
    return kOfxStatOK;
}

static OfxStatus get_double_n(OfxPropertySetHandle h, const char *name, int count, double *values) {
    for (int i = 0; i < count; ++i)
        if (get_double(h, name, i, &values[i]) != kOfxStatOK)
            return kOfxStatErrUnknown;
    return kOfxStatOK;
}

static OfxStatus get_int_n(OfxPropertySetHandle h, const char *name, int count, int *values) {
    for (int i = 0; i < count; ++i)
        if (get_int(h, name, i, &values[i]) != kOfxStatOK)
            return kOfxStatErrUnknown;
    return kOfxStatOK;
}

static OfxPropertySuiteV1 property_suite = {
    .propSetPointer = set_pointer,
    .propSetString = set_string,
    .propSetDouble = set_double,
    .propSetInt = set_int,
    .propGetPointer = get_pointer,
    .propGetString = get_string,
    .propGetDouble = get_double,
    .propGetInt = get_int,
    .propGetDoubleN = get_double_n,
    .propGetIntN = get_int_n,
};

// ---------------------------------------------------------------- parameters

#define MAX_PARAMS 64

typedef struct {
    char name[64];
    char type[64];
    PropSet props;
    double value;
} Param;

static Param params[MAX_PARAMS];
static int param_count;

static int is_int(const Param *p) {
    return strcmp(p->type, kOfxParamTypeChoice) == 0 ||
           strcmp(p->type, kOfxParamTypeBoolean) == 0 || strcmp(p->type, kOfxParamTypeInteger) == 0;
}

static OfxStatus param_define(OfxParamSetHandle set, const char *type, const char *name,
                              OfxPropertySetHandle *props) {
    (void)set;
    if (param_count == MAX_PARAMS)
        return kOfxStatErrMemory;
    Param *p = &params[param_count++];
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->type, sizeof p->type, "%s", type);
    p->props = (PropSet){.owner = name, .print = 1};
    printf("  param %s (%s)\n", name, type);
    if (props)
        *props = (OfxPropertySetHandle)&p->props;
    return kOfxStatOK;
}

static OfxStatus param_get_handle(OfxParamSetHandle set, const char *name, OfxParamHandle *param,
                                  OfxPropertySetHandle *props) {
    (void)set;
    for (int i = 0; i < param_count; ++i) {
        if (strcmp(params[i].name, name) == 0) {
            *param = (OfxParamHandle)&params[i];
            if (props)
                *props = (OfxPropertySetHandle)&params[i].props;
            return kOfxStatOK;
        }
    }
    return kOfxStatErrUnknown;
}

static OfxStatus param_get_value_at_time(OfxParamHandle handle, OfxTime time, ...) {
    const Param *p = (const Param *)handle;
    va_list args;
    va_start(args, time);
    if (is_int(p))
        *va_arg(args, int *) = (int)p->value;
    else
        *va_arg(args, double *) = p->value;
    va_end(args);
    return kOfxStatOK;
}

static OfxStatus param_set_value(OfxParamHandle handle, ...) {
    const Param *p = (const Param *)handle;
    va_list args;
    va_start(args, handle);
    if (strcmp(p->type, kOfxParamTypeString) == 0)
        printf("  %s = \"%s\"\n", p->name, va_arg(args, const char *));
    va_end(args);
    return kOfxStatOK;
}

static OfxParameterSuiteV1 parameter_suite = {
    .paramDefine = param_define,
    .paramGetHandle = param_get_handle,
    .paramGetValueAtTime = param_get_value_at_time,
    .paramSetValue = param_set_value,
};

// ---------------------------------------------------------------- clips and images

#define MAX_CLIPS 16

typedef struct {
    char name[64];
    PropSet props;
    PropSet image; // the image the host gives for this clip
    float *pixels; // width x height RGBA
} Clip;

static Clip clips[MAX_CLIPS];
static int clip_count;
static PropSet effect_props = {.owner = "effect", .print = 1};

static OfxStatus get_property_set(OfxImageEffectHandle effect, OfxPropertySetHandle *props) {
    (void)effect;
    *props = (OfxPropertySetHandle)&effect_props;
    return kOfxStatOK;
}

static OfxStatus get_param_set(OfxImageEffectHandle effect, OfxParamSetHandle *set) {
    (void)effect;
    *set = (OfxParamSetHandle)params;
    return kOfxStatOK;
}

static OfxStatus clip_define(OfxImageEffectHandle effect, const char *name,
                             OfxPropertySetHandle *props) {
    (void)effect;
    if (clip_count == MAX_CLIPS)
        return kOfxStatErrMemory;
    Clip *c = &clips[clip_count++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->props = (PropSet){.owner = c->name, .print = 1};
    printf("  clip %s\n", name);
    *props = (OfxPropertySetHandle)&c->props;
    return kOfxStatOK;
}

static OfxStatus clip_get_handle(OfxImageEffectHandle effect, const char *name,
                                 OfxImageClipHandle *clip, OfxPropertySetHandle *props) {
    (void)effect;
    for (int i = 0; i < clip_count; ++i) {
        if (strcmp(clips[i].name, name) == 0) {
            *clip = (OfxImageClipHandle)&clips[i];
            if (props)
                *props = (OfxPropertySetHandle)&clips[i].props;
            return kOfxStatOK;
        }
    }
    return kOfxStatErrUnknown;
}

static OfxStatus clip_get_image(OfxImageClipHandle clip, OfxTime time, const OfxRectD *region,
                                OfxPropertySetHandle *image) {
    (void)time;
    (void)region;
    Clip *c = (Clip *)clip;
    int connected = 0;
    get_int((OfxPropertySetHandle)&c->props, kOfxImageClipPropConnected, 0, &connected);
    if (!connected)
        return kOfxStatFailed;
    *image = (OfxPropertySetHandle)&c->image;
    return kOfxStatOK;
}

static OfxStatus clip_release_image(OfxPropertySetHandle image) {
    (void)image;
    return kOfxStatOK;
}

static OfxImageEffectSuiteV1 effect_suite = {
    .getPropertySet = get_property_set,
    .getParamSet = get_param_set,
    .clipDefine = clip_define,
    .clipGetHandle = clip_get_handle,
    .clipGetImage = clip_get_image,
    .clipReleaseImage = clip_release_image,
};

static OfxStatus message(void *handle, const char *type, const char *id, const char *format, ...) {
    (void)handle;
    (void)id;
    va_list args;
    va_start(args, format);
    printf("  message (%s): ", type);
    vprintf(format, args);
    printf("\n");
    va_end(args);
    return kOfxStatOK;
}

static OfxMessageSuiteV1 message_suite = {.message = message};

static const void *fetch_suite(OfxPropertySetHandle host, const char *name, int version) {
    (void)host;
    (void)version;
    if (strcmp(name, kOfxPropertySuite) == 0)
        return &property_suite;
    if (strcmp(name, kOfxParameterSuite) == 0)
        return &parameter_suite;
    if (strcmp(name, kOfxImageEffectSuite) == 0)
        return &effect_suite;
    if (strcmp(name, kOfxMessageSuite) == 0)
        return &message_suite;
    return NULL;
}

// ---------------------------------------------------------------- run

// A photo-like value: gradients, a disc and a box with hard edges, a little noise; alpha 1.
static float photo_value(int x, int y, int k, uint32_t *seed) {
    if (k == 3)
        return 1.0f;
    *seed = *seed * 1664525u + 1013904223u;
    const float noise = ((float)(*seed >> 8) / 16777216.0f - 0.5f) * 0.04f;
    const float dx = (float)x / (float)width - 0.39f, dy = (float)y / (float)height - 0.46f;
    if (dx * dx * 256.0f / 81.0f + dy * dy < 0.077f) // a disc on a 16:9 image
        return 0.9f - 0.3f * (float)k + noise;
    if (x > width * 68 / 100 && x < width * 88 / 100 && y > height / 7 && y < height * 5 / 6)
        return 0.1f + 0.2f * (float)k + noise;
    return 0.2f + 0.5f * (float)x / (float)width * (float)(k + 1) / 3.0f +
           0.2f * (float)y / (float)height + noise;
}

// Fills every input with its own pattern (or the photo-like image); the Output starts black.
static void make_images(int argc, char **argv) {
    for (int c = 0; c < clip_count; ++c) {
        Clip *clip = &clips[c];
        const int output = strcmp(clip->name, kOfxImageEffectOutputClipName) == 0;
        const int w = width + 2 * pad, h = height + 2 * pad;
        clip->pixels = calloc((size_t)w * h * 4, sizeof(float));
        uint32_t seed = 1u + (uint32_t)c;
        for (int y = 0; y < h && !output; ++y)
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < 4; ++k)
                    clip->pixels[((size_t)y * w + x) * 4 + k] =
                        photo ? photo_value(x, y, k, &seed)
                              : (float)((x * (7 + c) + y * 13 + k * 5) % 17) / 16.0f;

        int connected = 1;
        for (int a = 3; a < argc; ++a)
            if (argv[a][0] == '!' && strcmp(argv[a] + 1, clip->name) == 0)
                connected = 0;
        OfxPropertySetHandle props = (OfxPropertySetHandle)&clip->props;
        clip->props.print = 0;
        set_int(props, kOfxImageClipPropConnected, 0, connected);

        OfxPropertySetHandle image = (OfxPropertySetHandle)&clip->image;
        clip->image = (PropSet){.owner = "image"};
        set_pointer(image, kOfxImagePropData, 0, clip->pixels);
        set_int(image, kOfxImagePropRowBytes, 0, w * 4 * (int)sizeof(float));
        const int bounds[4] = {-pad, -pad, width + pad, height + pad};
        for (int i = 0; i < 4; ++i)
            set_int(image, kOfxImagePropBounds, i, bounds[i]);
    }
}

// Every parameter at its default, then the name=value arguments.
static void set_values(int argc, char **argv) {
    for (int i = 0; i < param_count; ++i)
        get_double((OfxPropertySetHandle)&params[i].props, kOfxParamPropDefault, 0,
                   &params[i].value);
    for (int a = 3; a < argc; ++a) {
        char name[64];
        double value;
        if (sscanf(argv[a], "%63[^=]=%lf", name, &value) != 2)
            continue;
        Param *p = NULL;
        for (int i = 0; i < param_count; ++i)
            if (strcmp(params[i].name, name) == 0)
                p = &params[i];
        if (p)
            p->value = value;
        else
            printf("unknown parameter %s\n", name);
    }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int check(const char *action, OfxStatus status) {
    const int ok = status == kOfxStatOK || status == kOfxStatReplyDefault;
    printf("%s: %s (%d)\n", action, ok ? "ok" : "FAILED", status);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <plugin.ofx> <effect id> [name=value ...] [!Clip ...] [--size WxH] "
                "[--frames N] [--save file]\n",
                argv[0]);
        return 2;
    }
    int frames = 1;
    const char *save = NULL;
    for (int a = 3; a + 1 < argc; ++a) {
        if (strcmp(argv[a], "--size") == 0 && sscanf(argv[a + 1], "%dx%d", &width, &height) == 2 &&
            width > 0 && height > 0)
            photo = 1;
        if (strcmp(argv[a], "--frames") == 0)
            frames = atoi(argv[a + 1]) > 0 ? atoi(argv[a + 1]) : 1;
        if (strcmp(argv[a], "--save") == 0)
            save = argv[a + 1];
        if (strcmp(argv[a], "--pad") == 0)
            pad = atoi(argv[a + 1]) > 0 ? atoi(argv[a + 1]) : 0;
    }
    void *library = dlopen(argv[1], RTLD_NOW);
    if (!library) {
        fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror());
        return 1;
    }
    int (*plugin_count)(void) = (int (*)(void))dlsym(library, "OfxGetNumberOfPlugins");
    OfxPlugin *(*get_plugin)(int) = (OfxPlugin * (*)(int)) dlsym(library, "OfxGetPlugin");
    OfxPlugin *plugin = NULL;
    for (int i = 0; plugin_count && get_plugin && i < plugin_count(); ++i)
        if (strcmp(get_plugin(i)->pluginIdentifier, argv[2]) == 0)
            plugin = get_plugin(i);
    if (!plugin) {
        fprintf(stderr, "no effect %s in %s\n", argv[2], argv[1]);
        dlclose(library);
        return 1;
    }

    PropSet host_props = {.owner = "host"};
    OfxHost host = {(OfxPropertySetHandle)&host_props, fetch_suite};
    PropSet context = {.owner = "context"};
    set_string((OfxPropertySetHandle)&context, kOfxImageEffectPropContext, 0,
               kOfxImageEffectContextGeneral);
    PropSet render_args = {.owner = "render"};
    OfxPropertySetHandle r = (OfxPropertySetHandle)&render_args;
    set_double(r, kOfxPropTime, 0, 1.0);
    set_double(r, kOfxImageEffectPropRenderScale, 0, 1.0);
    set_double(r, kOfxImageEffectPropRenderScale, 1, 1.0);
    const int window[4] = {0, 0, width, height};
    for (int i = 0; i < 4; ++i)
        set_int(r, kOfxImageEffectPropRenderWindow, i, window[i]);

    const void *effect = &effect_props; // the plugin only passes it back
    plugin->setHost(&host);
    int ok = check("load", plugin->mainEntry(kOfxActionLoad, NULL, NULL, NULL));
    ok = ok && check("describe", plugin->mainEntry(kOfxActionDescribe, effect, NULL, NULL));
    ok = ok && check("describe in context",
                     plugin->mainEntry(kOfxImageEffectActionDescribeInContext, effect,
                                       (OfxPropertySetHandle)&context, NULL));
    if (ok) {
        effect_props.print = 0;
        set_double((OfxPropertySetHandle)&effect_props, kOfxImageEffectPropProjectExtent, 0, width);
        set_double((OfxPropertySetHandle)&effect_props, kOfxImageEffectPropProjectExtent, 1,
                   height);
        set_values(argc, argv);
        make_images(argc, argv);
    }
    ok = ok &&
         check("create instance", plugin->mainEntry(kOfxActionCreateInstance, effect, NULL, NULL));
    for (int f = 0; ok && f < frames; ++f) {
        const double start = now_ms();
        ok = check("render", plugin->mainEntry(kOfxImageEffectActionRender, effect, r, NULL));
        if (frames > 1)
            printf("render %d of %d x %d: %.1f ms\n", f + 1, width, height, now_ms() - start);
    }
    check("unload", plugin->mainEntry(kOfxActionUnload, NULL, NULL, NULL));

    for (int c = 0; ok && save && c < clip_count; ++c) {
        if (strcmp(clips[c].name, kOfxImageEffectOutputClipName) != 0)
            continue;
        FILE *f = fopen(save, "wb");
        const size_t floats = (size_t)(width + 2 * pad) * (height + 2 * pad) * 4;
        ok = f && fwrite(clips[c].pixels, sizeof(float), floats, f) == floats;
        if (f)
            fclose(f);
        if (!ok)
            printf("cannot write %s\n", save);
    }

    for (int c = 0; c < clip_count; ++c)
        free(clips[c].pixels);
    dlclose(library);
    return ok ? 0 : 1;
}
