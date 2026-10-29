// A minimal OFX host, to run the plugin outside Nuke or Flame (tools/sanitize.sh runs it
// on a sanitizer build). It loads the plugin, then for one effect: describes it, creates
// an instance and renders one frame of a test pattern with every input connected.
//
//   test_host <plugin.ofx> <effect id> [name=value ...] [!Clip ...]
//
//   name=value   sets a parameter (choices and checkboxes by number)
//   !Clip        leaves that input unconnected, e.g. !Mask
//
// Prints what the effect declares and the result of each action; exits with 1 if an
// action fails. Only the host functions MillSaph uses are implemented.

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

// Odd sizes, to catch mistakes with odd rows and columns.
#define WIDTH 97
#define HEIGHT 61

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
    float *pixels; // WIDTH x HEIGHT RGBA
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

// Fills every input with its own pattern; the Output starts black.
static void make_images(int argc, char **argv) {
    for (int c = 0; c < clip_count; ++c) {
        Clip *clip = &clips[c];
        const int output = strcmp(clip->name, kOfxImageEffectOutputClipName) == 0;
        clip->pixels = calloc((size_t)WIDTH * HEIGHT * 4, sizeof(float));
        for (int y = 0; y < HEIGHT && !output; ++y)
            for (int x = 0; x < WIDTH; ++x)
                for (int k = 0; k < 4; ++k)
                    clip->pixels[((size_t)y * WIDTH + x) * 4 + k] =
                        (float)((x * (7 + c) + y * 13 + k * 5) % 17) / 16.0f;

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
        set_int(image, kOfxImagePropRowBytes, 0, WIDTH * 4 * (int)sizeof(float));
        const int bounds[4] = {0, 0, WIDTH, HEIGHT};
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

static int check(const char *action, OfxStatus status) {
    const int ok = status == kOfxStatOK || status == kOfxStatReplyDefault;
    printf("%s: %s (%d)\n", action, ok ? "ok" : "FAILED", status);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <plugin.ofx> <effect id> [name=value ...] [!Clip ...]\n",
                argv[0]);
        return 2;
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
    const int window[4] = {0, 0, WIDTH, HEIGHT};
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
        set_double((OfxPropertySetHandle)&effect_props, kOfxImageEffectPropProjectExtent, 0, WIDTH);
        set_double((OfxPropertySetHandle)&effect_props, kOfxImageEffectPropProjectExtent, 1,
                   HEIGHT);
        set_values(argc, argv);
        make_images(argc, argv);
    }
    ok = ok &&
         check("create instance", plugin->mainEntry(kOfxActionCreateInstance, effect, NULL, NULL));
    ok = ok && check("render", plugin->mainEntry(kOfxImageEffectActionRender, effect, r, NULL));
    check("unload", plugin->mainEntry(kOfxActionUnload, NULL, NULL, NULL));

    for (int c = 0; c < clip_count; ++c)
        free(clips[c].pixels);
    dlclose(library);
    return ok ? 0 : 1;
}
