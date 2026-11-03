// OFX adapter: makes an OFX plugin of each effect in ms_effects (see effect.h). All
// the OFX calls of an effect are here, so the effects themselves know nothing of OFX.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/log.h"
#include "common/mutex.h"
#include "engine/engine.h"
#include "ofx/host.h"
#include "ofx/image.h"
#include "ofxOld.h" // kOfxParamDoubleTypeNormalisedX/Y: deprecated, but hosts show them in pixels

#define MAX_EFFECTS 16 // number of entry points below
#define MAX_PARAMS 64

static OfxStatus describe(const MsEffect *e, OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    ms_effect->getPropertySet(effect, &props);
    ms_prop->propSetString(props, kOfxPropLabel, 0, e->label);
    ms_prop->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, MS_GROUP);
    ms_prop->propSetString(props, kOfxPropPluginDescription, 0, e->description);
    // With inputs besides Source, "general" only: hosts like Nuke pick "filter" when
    // offered and then show only one input.
    int contexts = 0;
    if (e->clip_count == 1)
        ms_prop->propSetString(props, kOfxImageEffectPropSupportedContexts, contexts++,
                               kOfxImageEffectContextFilter);
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedContexts, contexts,
                           kOfxImageEffectContextGeneral);

    // Floats and 16-bit floats (the kernels compute in floats: engine/run.c converts); the
    // host converts other depths. One depth for every image of a render.
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthHalf);
    ms_prop->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);

    // Whole frames, no tiling.
    ms_prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    ms_prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                           kOfxImageEffectRenderFullySafe);
    return kOfxStatOK;
}

static void define_param(OfxParamSetHandle params, const MsParam *p) {
    static const char *const types[] = {
        [MS_PARAM_DOUBLE] = kOfxParamTypeDouble, [MS_PARAM_WIDTH] = kOfxParamTypeDouble,
        [MS_PARAM_HEIGHT] = kOfxParamTypeDouble, [MS_PARAM_ANGLE] = kOfxParamTypeDouble,
        [MS_PARAM_CHOICE] = kOfxParamTypeChoice, [MS_PARAM_BOOL] = kOfxParamTypeBoolean,
        [MS_PARAM_TEXT] = kOfxParamTypeString,
    };
    OfxPropertySetHandle props;
    if (ms_param->paramDefine(params, types[p->type], p->name, &props) != kOfxStatOK)
        return;
    ms_prop->propSetString(props, kOfxPropLabel, 0, p->label);
    if (p->hint)
        ms_prop->propSetString(props, kOfxParamPropHint, 0, p->hint);

    switch (p->type) {
    case MS_PARAM_CHOICE:
        for (int i = 0; i < p->entry_count; ++i)
            ms_prop->propSetString(props, kOfxParamPropChoiceOption, i, p->entries[i]);
        ms_prop->propSetInt(props, kOfxParamPropDefault, 0, (int)p->def);
        return;
    case MS_PARAM_BOOL:
        ms_prop->propSetInt(props, kOfxParamPropDefault, 0, (int)p->def);
        return;
    case MS_PARAM_TEXT: // read-only, not saved, does not trigger renders
        ms_prop->propSetInt(props, kOfxParamPropEnabled, 0, 0);
        ms_prop->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 0);
        ms_prop->propSetInt(props, kOfxParamPropPersistant, 0, 0);
        return;
    case MS_PARAM_WIDTH:
        ms_prop->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeNormalisedX);
        break;
    case MS_PARAM_HEIGHT:
        ms_prop->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeNormalisedY);
        break;
    case MS_PARAM_ANGLE:
        ms_prop->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeAngle);
        break;
    case MS_PARAM_DOUBLE:
        break;
    }
    ms_prop->propSetDouble(props, kOfxParamPropDefault, 0, p->def);
    if (p->min != 0.0 || p->max != 0.0) {
        if (p->min > -MS_UNBOUNDED)
            ms_prop->propSetDouble(props, kOfxParamPropMin, 0, p->min);
        if (p->max < MS_UNBOUNDED)
            ms_prop->propSetDouble(props, kOfxParamPropMax, 0, p->max);
    }
    if (p->display_min != 0.0 || p->display_max != 0.0) {
        ms_prop->propSetDouble(props, kOfxParamPropDisplayMin, 0, p->display_min);
        ms_prop->propSetDouble(props, kOfxParamPropDisplayMax, 0, p->display_max);
    }
}

static OfxStatus describe_in_context(const MsEffect *e, OfxImageEffectHandle effect,
                                     OfxPropertySetHandle in_args) {
    OfxParamSetHandle params;
    ms_define_clips(effect, in_args, e->clips, e->clip_count);
    if (ms_effect->getParamSet(effect, &params) != kOfxStatOK)
        return kOfxStatFailed;
    for (int i = 0; i < e->param_count; ++i)
        define_param(params, &e->params[i]);
    return kOfxStatOK;
}

// Fills the TEXT parameters (MS_Test shows the GPU in use this way, which also starts
// the GPU, so problems show up straight away).
static OfxStatus create_instance(const MsEffect *e, OfxImageEffectHandle effect) {
    OfxParamSetHandle params;
    OfxParamHandle param;
    if (ms_effect->getParamSet(effect, &params) != kOfxStatOK)
        return kOfxStatOK;
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type == MS_PARAM_TEXT && p->text &&
            ms_param->paramGetHandle(params, p->name, &param, NULL) == kOfxStatOK)
            ms_param->paramSetValue(param, p->text());
    }
    return kOfxStatOK;
}

// Parameter values at `time` (0 if the parameter does not exist).
static double get_double(OfxParamSetHandle params, const char *name, OfxTime time) {
    OfxParamHandle param;
    double value = 0.0;
    if (ms_param->paramGetHandle(params, name, &param, NULL) == kOfxStatOK)
        ms_param->paramGetValueAtTime(param, time, &value);
    return value;
}

static int get_int(OfxParamSetHandle params, const char *name, OfxTime time) {
    OfxParamHandle param;
    int value = 0;
    if (ms_param->paramGetHandle(params, name, &param, NULL) == kOfxStatOK)
        ms_param->paramGetValueAtTime(param, time, &value);
    return value;
}

// The project frame in pixels of this render: project size x render scale.
static void frame_size(OfxImageEffectHandle effect, OfxPropertySetHandle in_args, double *width,
                       double *height) {
    OfxPropertySetHandle props;
    double scale[2] = {1.0, 1.0}, extent[2] = {0.0, 0.0};
    ms_prop->propGetDoubleN(in_args, kOfxImageEffectPropRenderScale, 2, scale);
    if (ms_effect->getPropertySet(effect, &props) == kOfxStatOK)
        ms_prop->propGetDoubleN(props, kOfxImageEffectPropProjectExtent, 2, extent);
    *width = extent[0] * scale[0];
    *height = extent[1] * scale[1];
}

static int clip_connected(OfxImageEffectHandle effect, const char *name) {
    OfxImageClipHandle clip;
    OfxPropertySetHandle props;
    int connected = 0;
    if (ms_effect->clipGetHandle(effect, name, &clip, &props) == kOfxStatOK)
        ms_prop->propGetInt(props, kOfxImageClipPropConnected, 0, &connected);
    return connected;
}

typedef struct {
    const MsEffect *effect;
    const double *values;
    const int *connected;
    double frame_width, frame_height;
} Job;

// The settings of the first renders of a session (WIDTH and HEIGHT in pixels) and the API
// the effect last ran on, in the log: what a slow render was asked to do.
#define REPORTED_SETTINGS 5000

static void log_settings(const MsEffect *e, const double *values) {
    static int reported;
    static MsMutex lock = MS_MUTEX_INIT;
    ms_mutex_lock(&lock);
    const int number = ++reported;
    ms_mutex_unlock(&lock);
    if (number > REPORTED_SETTINGS)
        return;
    char line[1024] = "";
    size_t used = 0;
    for (int i = 0; i < e->param_count && used < sizeof line; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type != MS_PARAM_TEXT)
            used += (size_t)snprintf(line + used, sizeof line - used, " %s=%g", p->name, values[i]);
    }
    const MsApi api = ms_api_used();
    ms_log("settings %s:%s; ran on %s", e->label, line, ms_api_name(api));
}

static int run(const MsInput *inputs, int input_count, int half, int width, int height,
               void *output, ptrdiff_t output_row, char *err, size_t err_size, void *user) {
    (void)input_count;
    const Job *job = user;
    const MsRender r = {job->values,      job->connected,    inputs, width,      height,
                        job->frame_width, job->frame_height, output, output_row, err,
                        err_size,         half};
    const int ok = job->effect->render(&r);
    log_settings(job->effect, job->values);
    return ok;
}

// The parameter values at the time of `in_args` (WIDTH and HEIGHT in pixels of the frame
// `px` x `py`) and which inputs are connected. Returns 0 when the host does not give them.
static int read_values(const MsEffect *e, OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                       OfxTime *time, double *px, double *py, double *values, int *connected) {
    OfxParamSetHandle params;
    if (e->param_count > MAX_PARAMS || e->clip_count > MS_MAX_CLIPS ||
        ms_prop->propGetDouble(in_args, kOfxPropTime, 0, time) != kOfxStatOK ||
        ms_effect->getParamSet(effect, &params) != kOfxStatOK)
        return 0;
    frame_size(effect, in_args, px, py);
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        switch (p->type) {
        case MS_PARAM_CHOICE:
        case MS_PARAM_BOOL:
            values[i] = get_int(params, p->name, *time);
            break;
        case MS_PARAM_TEXT:
            values[i] = 0.0;
            break;
        case MS_PARAM_WIDTH:
            values[i] = get_double(params, p->name, *time) * *px;
            break;
        case MS_PARAM_HEIGHT:
            values[i] = get_double(params, p->name, *time) * *py;
            break;
        case MS_PARAM_DOUBLE:
        case MS_PARAM_ANGLE:
            values[i] = get_double(params, p->name, *time);
            break;
        }
    }
    for (int c = 0; c < e->clip_count; ++c)
        connected[c] = clip_connected(effect, e->clips[c].clip);
    return 1;
}

static OfxStatus render(const MsEffect *e, OfxImageEffectHandle effect,
                        OfxPropertySetHandle in_args) {
    OfxTime time;
    double px, py; // WIDTH and HEIGHT values are fractions of these
    double values[MAX_PARAMS];
    int connected[MS_MAX_CLIPS];
    if (!read_values(e, effect, in_args, &time, &px, &py, values, connected))
        return kOfxStatFailed;
    Job job = {e, values, connected, px, py};
    return ms_render_with(effect, in_args, e->clips, connected, e->clip_count, run, &job);
}

// Settings that leave the Source as it is: the host uses the Source (at the same time)
// without rendering.
static OfxStatus is_identity(const MsEffect *e, OfxImageEffectHandle effect,
                             OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {
    OfxTime time;
    double px, py;
    double values[MAX_PARAMS];
    int connected[MS_MAX_CLIPS];
    if (!e->identity || !read_values(e, effect, in_args, &time, &px, &py, values, connected) ||
        !e->identity(values, connected))
        return kOfxStatReplyDefault;
    ms_prop->propSetString(out_args, kOfxPropName, 0, e->clips[0].clip);
    ms_prop->propSetDouble(out_args, kOfxPropTime, 0, time);
    return kOfxStatOK;
}

static OfxStatus main_entry(const MsEffect *e, const char *action, const void *handle,
                            OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (strcmp(action, kOfxActionLoad) == 0)
        return ms_load();
    if (strcmp(action, kOfxActionUnload) == 0)
        return ms_unload();
    if (strcmp(action, kOfxActionDescribe) == 0)
        return describe(e, effect);
    if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
        return describe_in_context(e, effect, in_args);
    if (strcmp(action, kOfxActionCreateInstance) == 0)
        return create_instance(e, effect);
    if (strcmp(action, kOfxImageEffectActionRender) == 0)
        return render(e, effect, in_args);
    if (strcmp(action, kOfxImageEffectActionIsIdentity) == 0)
        return is_identity(e, effect, in_args, out_args);
    return kOfxStatReplyDefault;
}

// OFX does not tell the entry point which plugin is called, so each plugin gets its
// own entry point, which only passes its effect on. Add lines for more than MAX_EFFECTS.
#define ENTRY(i)                                                                                   \
    static OfxStatus entry_##i(const char *action, const void *handle,                             \
                               OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {      \
        return main_entry(ms_effects[i], action, handle, in_args, out_args);                       \
    }
// clang-format off
ENTRY(0) ENTRY(1) ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6) ENTRY(7)
ENTRY(8) ENTRY(9) ENTRY(10) ENTRY(11) ENTRY(12) ENTRY(13) ENTRY(14) ENTRY(15)

static OfxPluginEntryPoint *const entries[MAX_EFFECTS] = {
    entry_0, entry_1, entry_2,  entry_3,  entry_4,  entry_5,  entry_6,  entry_7,
    entry_8, entry_9, entry_10, entry_11, entry_12, entry_13, entry_14, entry_15,
};
// clang-format on

static OfxPlugin plugins[MAX_EFFECTS];

OfxPlugin *ms_ofx_plugin(int index) {
    if (index < 0 || index >= ms_effect_count || index >= MAX_EFFECTS)
        return NULL;
    plugins[index] = (OfxPlugin){
        kOfxImageEffectPluginApi, 1, ms_effects[index]->id, 1, 0, ms_set_host, entries[index]};
    return &plugins[index];
}
