// OFX adapter: makes an OFX plugin of each effect in ms_effects (see effect.h). All
// the OFX calls of an effect are here, so the effects themselves know nothing of OFX.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "engine/engine.h"
#include "ofx/host.h"
#include "ofx/image.h"
#include "ofxOld.h" // kOfxParamDoubleTypeNormalisedX/Y: deprecated, but hosts show them in pixels

#define MAX_EFFECTS 32 // number of entry points below

static OfxPluginEntryPoint *ms_overlay_entry(int index); // (below)

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

    // A viewer overlay (ofx/overlay.c)
    if (e->overlay[0] && e->overlay[1])
        for (int i = 0; i < ms_effect_count && i < MAX_EFFECTS; ++i)
            if (ms_effects[i] == e)
                ms_prop->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0,
                                        (void *)ms_overlay_entry(i));

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
        [MS_PARAM_TEXT] = kOfxParamTypeString,   [MS_PARAM_RGB] = kOfxParamTypeRGB,
        [MS_PARAM_STRING] = kOfxParamTypeString, [MS_PARAM_POINT] = kOfxParamTypeDouble2D,
        [MS_PARAM_BUTTON] = kOfxParamTypePushButton,
    };
    OfxPropertySetHandle props;
    if (p->type == MS_PARAM_RGB_PART || p->type == MS_PARAM_POINT_Y) // part of the one before
        return;
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
    case MS_PARAM_RGB: // the defaults of red, green and blue: this entry and the next two
        for (int k = 0; k < 3; ++k)
            ms_prop->propSetDouble(props, kOfxParamPropDefault, k, p[k].def);
        return;
    case MS_PARAM_RGB_PART:
    case MS_PARAM_POINT_Y:
        return;
    case MS_PARAM_POINT: // absolute, its default in fractions of the frame; the host's handle
        ms_prop->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
        ms_prop->propSetInt(props, kOfxParamPropUseHostOverlayHandle, 0, 1);
        ms_prop->propSetString(props, kOfxParamPropDefaultCoordinateSystem, 0,
                               kOfxParamCoordinatesNormalised);
        ms_prop->propSetDouble(props, kOfxParamPropDefault, 0, p[0].def);
        ms_prop->propSetDouble(props, kOfxParamPropDefault, 1, p[1].def);
        return;
    case MS_PARAM_TEXT: // read-only, not saved, does not trigger renders
        ms_prop->propSetInt(props, kOfxParamPropEnabled, 0, 0);
        ms_prop->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 0);
        ms_prop->propSetInt(props, kOfxParamPropPersistant, 0, 0);
        return;
    case MS_PARAM_STRING:
        ms_prop->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsMultiLine);
        ms_prop->propSetString(props, kOfxParamPropDefault, 0, p->def_text ? p->def_text : "");
        return;
    case MS_PARAM_BUTTON:
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
static void get_rgb(OfxParamSetHandle params, const char *name, OfxTime time, double *rgb) {
    OfxParamHandle param;
    rgb[0] = rgb[1] = rgb[2] = 0.0;
    if (ms_param->paramGetHandle(params, name, &param, NULL) == kOfxStatOK)
        ms_param->paramGetValueAtTime(param, time, &rgb[0], &rgb[1], &rgb[2]);
}

static void get_point(OfxParamSetHandle params, const char *name, OfxTime time, double *xy) {
    OfxParamHandle param;
    xy[0] = xy[1] = 0.0;
    if (ms_param->paramGetHandle(params, name, &param, NULL) == kOfxStatOK)
        ms_param->paramGetValueAtTime(param, time, &xy[0], &xy[1]);
}

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
                       double *height, double *scale) {
    OfxPropertySetHandle props;
    double extent[2] = {0.0, 0.0};
    scale[0] = scale[1] = 1.0;
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

static const char *get_text(OfxParamSetHandle params, const char *name, OfxTime time) {
    OfxParamHandle param;
    char *value = NULL;
    if (ms_param->paramGetHandle(params, name, &param, NULL) == kOfxStatOK)
        ms_param->paramGetValueAtTime(param, time, &value);
    return value;
}

// The parameter values at `time` (WIDTH and HEIGHT in pixels of the frame `px` x `py`,
// POINT ones x `scale`; STRING ones in `texts`, the host's own strings).
static void fill_values(const MsEffect *e, OfxParamSetHandle params, OfxTime time, double px,
                        double py, const double scale[2], double *values, const char **texts) {
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        texts[i] = NULL;
        switch (p->type) {
        case MS_PARAM_CHOICE:
        case MS_PARAM_BOOL:
            values[i] = get_int(params, p->name, time);
            break;
        case MS_PARAM_TEXT:
            values[i] = 0.0;
            break;
        case MS_PARAM_STRING:
            values[i] = 0.0;
            texts[i] = get_text(params, p->name, time);
            break;
        case MS_PARAM_RGB: // red, green and blue in this entry and the next two
            get_rgb(params, p->name, time, &values[i]);
            break;
        case MS_PARAM_RGB_PART: // read with the MS_PARAM_RGB before
        case MS_PARAM_POINT_Y:  // with the MS_PARAM_POINT before
            break;
        case MS_PARAM_BUTTON:
            values[i] = 0.0;
            break;
        case MS_PARAM_POINT: // canonical coordinates: x the render scale
            get_point(params, p->name, time, &values[i]);
            values[i] *= scale[0];
            values[i + 1] *= scale[1];
            break;
        case MS_PARAM_WIDTH:
            values[i] = get_double(params, p->name, time) * px;
            break;
        case MS_PARAM_HEIGHT:
            values[i] = get_double(params, p->name, time) * py;
            break;
        case MS_PARAM_DOUBLE:
        case MS_PARAM_ANGLE:
            values[i] = get_double(params, p->name, time);
            break;
        }
    }
}

// The parameter values at the time of `in_args` (as fill_values; WIDTH and HEIGHT in pixels
// of the frame `px` x `py`) and which inputs are connected. Returns 0 when the host does not
// give them.
static int read_values(const MsEffect *e, OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                       OfxTime *time, double *px, double *py, double *values, const char **texts,
                       int *connected) {
    OfxParamSetHandle params;
    if (e->param_count > MS_MAX_PARAMS || e->clip_count > MS_MAX_CLIPS ||
        ms_prop->propGetDouble(in_args, kOfxPropTime, 0, time) != kOfxStatOK ||
        ms_effect->getParamSet(effect, &params) != kOfxStatOK)
        return 0;
    double scale[2];
    frame_size(effect, in_args, px, py, scale);
    fill_values(e, params, *time, *px, *py, scale, values, texts);
    for (int c = 0; c < e->clip_count; ++c)
        connected[c] = clip_connected(effect, e->clips[c].clip);
    return 1;
}

// MsRender.values_at: the values at another time of the render's instance (its STRING ones
// left as they are at the render's time).
typedef struct {
    const MsEffect *e;
    OfxImageEffectHandle effect;
    OfxPropertySetHandle in_args;
} RenderCaller;

static int values_at(const MsRender *r, double time, double *values) {
    const RenderCaller *c = r->caller;
    OfxParamSetHandle params;
    const char *texts[MS_MAX_PARAMS];
    double px, py, scale[2];
    if (ms_effect->getParamSet(c->effect, &params) != kOfxStatOK)
        return 0;
    frame_size(c->effect, c->in_args, &px, &py, scale);
    fill_values(c->e, params, time, px, py, scale, values, texts);
    return 1;
}

static OfxStatus render(const MsEffect *e, OfxImageEffectHandle effect,
                        OfxPropertySetHandle in_args) {
    OfxTime time;
    double px, py; // WIDTH and HEIGHT values are fractions of these
    double values[MS_MAX_PARAMS];
    const char *texts[MS_MAX_PARAMS];
    int connected[MS_MAX_CLIPS];
    if (!read_values(e, effect, in_args, &time, &px, &py, values, texts, connected))
        return kOfxStatFailed;
    RenderCaller caller = {e, effect, in_args};
    MsRender r = {.values = values,
                  .connected = connected,
                  .frame_width = px,
                  .frame_height = py,
                  .texts = texts,
                  .time = time,
                  .values_at = values_at,
                  .caller = &caller};
    return ms_render_with(effect, in_args, e, &r);
}

// Settings that leave the Source as it is: the host uses the Source (at the same time)
// without rendering.
static OfxStatus is_identity(const MsEffect *e, OfxImageEffectHandle effect,
                             OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {
    OfxTime time;
    double px, py;
    double values[MS_MAX_PARAMS];
    const char *texts[MS_MAX_PARAMS];
    int connected[MS_MAX_CLIPS];
    if (!e->identity ||
        !read_values(e, effect, in_args, &time, &px, &py, values, texts, connected) ||
        !e->identity(values, connected))
        return kOfxStatReplyDefault;
    ms_prop->propSetString(out_args, kOfxPropName, 0, e->clips[0].clip);
    ms_prop->propSetDouble(out_args, kOfxPropTime, 0, time);
    return kOfxStatOK;
}

// MsHost on the parameters of one instance (a button's press, see MsHost).
static OfxParamHandle param_named(MsHost *host, const char *name) {
    OfxParamSetHandle params;
    OfxParamHandle param;
    if (ms_effect->getParamSet((OfxImageEffectHandle)host->data, &params) != kOfxStatOK ||
        ms_param->paramGetHandle(params, name, &param, NULL) != kOfxStatOK)
        return NULL;
    return param;
}

static const char *host_get_text(MsHost *host, const char *name) {
    OfxParamHandle param = param_named(host, name);
    char *value = NULL;
    if (param)
        ms_param->paramGetValue(param, &value);
    return value;
}

static int host_set_text(MsHost *host, const char *name, const char *text) {
    OfxParamHandle param = param_named(host, name);
    return param && ms_param->paramSetValue(param, text) == kOfxStatOK;
}

// (a POINT's y: its name with "_y", the host's second value)
static double host_get_value(MsHost *host, const char *name, int *ok) {
    char base[64];
    size_t n = strlen(name);
    const int y = n > 2 && strcmp(name + n - 2, "_y") == 0 && n - 2 < sizeof base;
    if (y) {
        memcpy(base, name, n - 2);
        base[n - 2] = 0;
    }
    OfxParamHandle param = param_named(host, y ? base : name);
    OfxPropertySetHandle props;
    char *type = NULL;
    *ok = param && ms_param->paramGetPropertySet(param, &props) == kOfxStatOK &&
          ms_prop->propGetString(props, kOfxParamPropType, 0, &type) == kOfxStatOK;
    if (!*ok)
        return 0.0;
    if (strcmp(type, kOfxParamTypeChoice) == 0 || strcmp(type, kOfxParamTypeBoolean) == 0 ||
        strcmp(type, kOfxParamTypeInteger) == 0) {
        int v = 0;
        ms_param->paramGetValue(param, &v);
        return v;
    }
    double v[3] = {0, 0, 0};
    ms_param->paramGetValue(param, &v[0], &v[1], &v[2]);
    return v[y];
}

static double host_value(MsHost *host, const char *name) {
    int ok;
    return host_get_value(host, name, &ok);
}

static int host_set_value(MsHost *host, const char *name, double value) {
    OfxParamHandle param = param_named(host, name);
    OfxPropertySetHandle props;
    char *type = NULL;
    if (!param || ms_param->paramGetPropertySet(param, &props) != kOfxStatOK ||
        ms_prop->propGetString(props, kOfxParamPropType, 0, &type) != kOfxStatOK)
        return 0;
    if (strcmp(type, kOfxParamTypeChoice) == 0 || strcmp(type, kOfxParamTypeBoolean) == 0 ||
        strcmp(type, kOfxParamTypeInteger) == 0)
        return ms_param->paramSetValue(param, (int)value) == kOfxStatOK;
    return ms_param->paramSetValue(param, value) == kOfxStatOK;
}

// A parameter changed: a button pressed runs its press (errors shown by the host).
static OfxStatus instance_changed(const MsEffect *e, OfxImageEffectHandle effect,
                                  OfxPropertySetHandle in_args) {
    char *name = NULL, *type = NULL;
    if (ms_prop->propGetString(in_args, kOfxPropType, 0, &type) != kOfxStatOK ||
        strcmp(type, kOfxTypeParameter) != 0 ||
        ms_prop->propGetString(in_args, kOfxPropName, 0, &name) != kOfxStatOK)
        return kOfxStatReplyDefault;
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type != MS_PARAM_BUTTON || !p->press || strcmp(p->name, name) != 0)
            continue;
        MsHost host = {host_get_text, host_set_text, host_value, host_set_value, effect};
        char err[512] = "";
        if (!p->press(&host, err, sizeof err) && ms_message)
            ms_message->message(effect, kOfxMessageError, NULL, "%s: %s", e->label, err);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
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
    if (strcmp(action, kOfxActionInstanceChanged) == 0)
        return instance_changed(e, effect, in_args);
    // An effect that changes over time by itself: the host renders every frame.
    if (strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0 && e->frame_varying) {
        ms_prop->propSetInt(out_args, kOfxImageEffectFrameVarying, 0, 1);
        return kOfxStatOK;
    }
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
ENTRY(16) ENTRY(17) ENTRY(18) ENTRY(19) ENTRY(20) ENTRY(21) ENTRY(22) ENTRY(23)
ENTRY(24) ENTRY(25) ENTRY(26) ENTRY(27) ENTRY(28) ENTRY(29) ENTRY(30) ENTRY(31)

// The same for the overlays' interacts.
#define OVERLAY(i)                                                                                 \
    static OfxStatus overlay_##i(const char *action, const void *handle,                           \
                                 OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {    \
        return ms_overlay_main(ms_effects[i], action, handle, in_args, out_args);                  \
    }
OVERLAY(0) OVERLAY(1) OVERLAY(2) OVERLAY(3) OVERLAY(4) OVERLAY(5) OVERLAY(6) OVERLAY(7)
OVERLAY(8) OVERLAY(9) OVERLAY(10) OVERLAY(11) OVERLAY(12) OVERLAY(13) OVERLAY(14) OVERLAY(15)
OVERLAY(16) OVERLAY(17) OVERLAY(18) OVERLAY(19) OVERLAY(20) OVERLAY(21) OVERLAY(22) OVERLAY(23)
OVERLAY(24) OVERLAY(25) OVERLAY(26) OVERLAY(27) OVERLAY(28) OVERLAY(29) OVERLAY(30) OVERLAY(31)

static OfxPluginEntryPoint *const overlays[MAX_EFFECTS] = {
    overlay_0, overlay_1, overlay_2,  overlay_3,  overlay_4,  overlay_5,  overlay_6,  overlay_7,
    overlay_8, overlay_9, overlay_10, overlay_11, overlay_12, overlay_13, overlay_14, overlay_15,
    overlay_16, overlay_17, overlay_18, overlay_19, overlay_20, overlay_21, overlay_22, overlay_23,
    overlay_24, overlay_25, overlay_26, overlay_27, overlay_28, overlay_29, overlay_30, overlay_31,
};

static OfxPluginEntryPoint *const entries[MAX_EFFECTS] = {
    entry_0, entry_1, entry_2,  entry_3,  entry_4,  entry_5,  entry_6,  entry_7,
    entry_8, entry_9, entry_10, entry_11, entry_12, entry_13, entry_14, entry_15,
    entry_16, entry_17, entry_18, entry_19, entry_20, entry_21, entry_22, entry_23,
    entry_24, entry_25, entry_26, entry_27, entry_28, entry_29, entry_30, entry_31,
};
// clang-format on

static OfxPluginEntryPoint *ms_overlay_entry(int index) { return overlays[index]; }

static OfxPlugin plugins[MAX_EFFECTS];

OfxPlugin *ms_ofx_plugin(int index) {
    if (index < 0 || index >= ms_effect_count || index >= MAX_EFFECTS)
        return NULL;
    plugins[index] = (OfxPlugin){
        kOfxImageEffectPluginApi, 1, ms_effects[index]->id, 1, 0, ms_set_host, entries[index]};
    return &plugins[index];
}
