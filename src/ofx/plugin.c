// OFX adapter: makes an OFX plugin of each effect in ms_effects (see effect.h). All
// the OFX calls of an effect are here, so the effects themselves know nothing of OFX.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/preset.h"
#include "engine/engine.h"
#include "ofx/host.h"
#include "ofx/image.h"
#include "ofxOld.h" // kOfxParamDoubleTypeNormalisedX/Y: deprecated, but hosts show them in pixels

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
    if (e->overlay && e->overlay_count > 0)
        for (int i = 0; i < ms_effect_count; ++i)
            if (ms_effects[i] == e)
                ms_prop->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0,
                                        (void *)ms_overlay_entry(i));

    // Settings that change whether the result varies over time: the host asks again
    // (clip_preferences) when one changes.
    for (int i = 0, n = 0; e->varying && i < e->param_count; ++i)
        if (!ms_param_part(e->params[i].type) && e->params[i].type != MS_PARAM_BUTTON &&
            e->params[i].type != MS_PARAM_TEXT)
            ms_prop->propSetString(props, kOfxImageEffectPropClipPreferencesSlaveParam, n++,
                                   e->params[i].name);

    // Whole frames, no tiling.
    ms_prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    ms_prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                           kOfxImageEffectRenderFullySafe);
    return kOfxStatOK;
}

// host_handle: the host draws POINT parameters' handles (not when the effect's overlay does).
static void define_param(OfxParamSetHandle params, const MsParam *p, int host_handle) {
    static const char *const types[] = {
        [MS_PARAM_DOUBLE] = kOfxParamTypeDouble,     [MS_PARAM_WIDTH] = kOfxParamTypeDouble,
        [MS_PARAM_HEIGHT] = kOfxParamTypeDouble,     [MS_PARAM_ANGLE] = kOfxParamTypeDouble,
        [MS_PARAM_CHOICE] = kOfxParamTypeChoice,     [MS_PARAM_BOOL] = kOfxParamTypeBoolean,
        [MS_PARAM_TEXT] = kOfxParamTypeString,       [MS_PARAM_RGB] = kOfxParamTypeRGB,
        [MS_PARAM_STRING] = kOfxParamTypeString,     [MS_PARAM_POINT] = kOfxParamTypeDouble2D,
        [MS_PARAM_BUTTON] = kOfxParamTypePushButton,
    };
    OfxPropertySetHandle props;
    if (ms_param_part(p->type)) // with the one before
        return;
    if (ms_param->paramDefine(params, types[p->type], p->name, &props) != kOfxStatOK)
        return;
    ms_prop->propSetString(props, kOfxPropLabel, 0, p->label);
    if (p->hint)
        ms_prop->propSetString(props, kOfxParamPropHint, 0, p->hint);
    if (p->hidden)
        ms_prop->propSetInt(props, kOfxParamPropSecret, 0, 1);

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
    case MS_PARAM_POINT: // absolute, its default in fractions of the frame
        ms_prop->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
        ms_prop->propSetInt(props, kOfxParamPropUseHostOverlayHandle, 0, host_handle);
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
    if (e->presets) { // first, as hosts show the parameters in order
        static const MsParam load = {.type = MS_PARAM_BUTTON,
                                     .name = MS_LOAD_PRESET,
                                     .label = "Load Preset",
                                     .hint = "Picks one of the effect's presets in a window."};
        define_param(params, &load, 1);
    }
    for (int i = 0; i < e->param_count; ++i)
        define_param(params, &e->params[i], !(e->overlay && e->overlay_count > 0));
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

// The parameter values at `time` (WIDTH and HEIGHT in pixels of the frame `px` x `py`,
// POINT ones x `scale`; STRING ones in `texts`, the host's own strings).
static void fill_values(const MsEffect *e, OfxParamSetHandle params, OfxTime time, double px,
                        double py, const double scale[2], double *values, const char **texts) {
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        texts[i] = NULL;
        if (ms_param_part(p->type))
            continue; // read with the MS_PARAM_RGB or MS_PARAM_POINT before
        // (0 where the parameter has no value or does not exist)
        const int parts = p->type == MS_PARAM_RGB ? 3 : p->type == MS_PARAM_POINT ? 2 : 1;
        for (int k = 0; k < parts; ++k)
            values[i + k] = 0.0;
        OfxParamHandle param;
        if (p->type == MS_PARAM_TEXT || p->type == MS_PARAM_BUTTON ||
            ms_param->paramGetHandle(params, p->name, &param, NULL) != kOfxStatOK)
            continue;
        int choice = 0;
        char *text = NULL;
        switch (p->type) {
        case MS_PARAM_CHOICE:
        case MS_PARAM_BOOL:
            ms_param->paramGetValueAtTime(param, time, &choice);
            values[i] = choice;
            break;
        case MS_PARAM_STRING:
            ms_param->paramGetValueAtTime(param, time, &text);
            texts[i] = text;
            break;
        case MS_PARAM_RGB: // red, green and blue in this entry and the next two
            ms_param->paramGetValueAtTime(param, time, &values[i], &values[i + 1], &values[i + 2]);
            break;
        case MS_PARAM_POINT: // canonical coordinates: x the render scale
            ms_param->paramGetValueAtTime(param, time, &values[i], &values[i + 1]);
            values[i] *= scale[0];
            values[i + 1] *= scale[1];
            break;
        case MS_PARAM_WIDTH:
        case MS_PARAM_HEIGHT:
        case MS_PARAM_DOUBLE:
        case MS_PARAM_ANGLE:
            ms_param->paramGetValueAtTime(param, time, &values[i]);
            values[i] *= p->type == MS_PARAM_WIDTH ? px : p->type == MS_PARAM_HEIGHT ? py : 1.0;
            break;
        default:
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

// The parameter `name` stands for, and which of its values: itself (0), or a part of it, a
// POINT's y (its name with "_y": 1), an RGB's green or blue ("_green" 1, "_blue" 2). Its host
// type in *type. NULL when there is none.
static OfxParamHandle param_part(MsHost *host, const char *name, int *index, char **type) {
    static const struct {
        const char *suffix;
        int index;
    } parts[] = {{"_y", 1}, {"_green", 1}, {"_blue", 2}};
    OfxParamHandle param = param_named(host, name);
    *index = 0;
    char base[96];
    const size_t n = strlen(name);
    for (size_t i = 0; !param && i < sizeof parts / sizeof parts[0]; ++i) {
        const size_t k = strlen(parts[i].suffix);
        if (n > k && n - k < sizeof base && strcmp(name + n - k, parts[i].suffix) == 0) {
            memcpy(base, name, n - k);
            base[n - k] = 0;
            param = param_named(host, base);
            *index = parts[i].index;
        }
    }
    OfxPropertySetHandle props;
    *type = NULL;
    if (!param || ms_param->paramGetPropertySet(param, &props) != kOfxStatOK ||
        ms_prop->propGetString(props, kOfxParamPropType, 0, type) != kOfxStatOK || !*type)
        return NULL;
    return param;
}

// 1 when the host keeps the parameter as an int (CHOICE, BOOL).
static int int_type(const char *type) {
    return strcmp(type, kOfxParamTypeChoice) == 0 || strcmp(type, kOfxParamTypeBoolean) == 0 ||
           strcmp(type, kOfxParamTypeInteger) == 0;
}

// (a part of a parameter as param_part)
static double host_get_value(MsHost *host, const char *name) {
    int index;
    char *type;
    OfxParamHandle param = param_part(host, name, &index, &type);
    if (!param)
        return 0.0;
    if (int_type(type)) {
        int v = 0;
        ms_param->paramGetValue(param, &v);
        return v;
    }
    double v[3] = {0, 0, 0};
    ms_param->paramGetValue(param, &v[0], &v[1], &v[2]);
    return v[index];
}

// (a part of a parameter as param_part: the parameter's other values kept)
static int host_set_value(MsHost *host, const char *name, double value) {
    int index;
    char *type;
    OfxParamHandle param = param_part(host, name, &index, &type);
    if (!param)
        return 0;
    if (int_type(type))
        return ms_param->paramSetValue(param, (int)value) == kOfxStatOK;
    if (strcmp(type, kOfxParamTypeDouble2D) == 0) {
        double v[2] = {0, 0};
        ms_param->paramGetValue(param, &v[0], &v[1]);
        v[index] = value;
        return ms_param->paramSetValue(param, v[0], v[1]) == kOfxStatOK;
    }
    if (strcmp(type, kOfxParamTypeRGB) == 0) {
        double v[3] = {0, 0, 0};
        ms_param->paramGetValue(param, &v[0], &v[1], &v[2]);
        v[index] = value;
        return ms_param->paramSetValue(param, v[0], v[1], v[2]) == kOfxStatOK;
    }
    return ms_param->paramSetValue(param, value) == kOfxStatOK;
}

// The project's frame in canonical pixels (POINT values).
static int host_frame_size(MsHost *host, double size[2]) {
    OfxPropertySetHandle props;
    return ms_effect->getPropertySet((OfxImageEffectHandle)host->data, &props) == kOfxStatOK &&
           ms_prop->propGetDouble(props, kOfxImageEffectPropProjectSize, 0, &size[0]) ==
               kOfxStatOK &&
           ms_prop->propGetDouble(props, kOfxImageEffectPropProjectSize, 1, &size[1]) == kOfxStatOK;
}

// A parameter changed: a button pressed runs its press (errors shown by the host).
static OfxStatus instance_changed(const MsEffect *e, OfxImageEffectHandle effect,
                                  OfxPropertySetHandle in_args) {
    char *name = NULL, *type = NULL;
    if (ms_prop->propGetString(in_args, kOfxPropType, 0, &type) != kOfxStatOK ||
        strcmp(type, kOfxTypeParameter) != 0 ||
        ms_prop->propGetString(in_args, kOfxPropName, 0, &name) != kOfxStatOK)
        return kOfxStatReplyDefault;
    MsHost host = {host_get_text,  host_set_text, host_get_value,
                   host_set_value, effect,        host_frame_size};
    char err[512] = "";
    if (e->presets && strcmp(name, MS_LOAD_PRESET) == 0) {
        if (!ms_load_preset(e, &host, err, sizeof err) && ms_message)
            ms_message->message(effect, kOfxMessageError, NULL, "%s: %s", e->label, err);
        return kOfxStatOK;
    }
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type != MS_PARAM_BUTTON || !p->press || strcmp(p->name, name) != 0)
            continue;
        if (!p->press(&host, err, sizeof err) && ms_message)
            ms_message->message(effect, kOfxMessageError, NULL, "%s: %s", e->label, err);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// An effect that changes over time by itself (always, or with these settings): the host
// renders every frame. (MsEffect.varying: the values at frame 0.)
static OfxStatus clip_preferences(const MsEffect *e, OfxImageEffectHandle effect,
                                  OfxPropertySetHandle out_args) {
    int varying = e->frame_varying;
    OfxParamSetHandle params;
    if (!varying && e->varying && e->param_count <= MS_MAX_PARAMS &&
        ms_effect->getParamSet(effect, &params) == kOfxStatOK) {
        double values[MS_MAX_PARAMS], scale[2] = {1.0, 1.0};
        const char *texts[MS_MAX_PARAMS];
        fill_values(e, params, 0.0, 1.0, 1.0, scale, values, texts);
        varying = e->varying(values);
    }
    if (!varying)
        return kOfxStatReplyDefault;
    ms_prop->propSetInt(out_args, kOfxImageEffectFrameVarying, 0, 1);
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
    if (strcmp(action, kOfxActionInstanceChanged) == 0)
        return instance_changed(e, effect, in_args);
    if (strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0)
        return clip_preferences(e, effect, out_args);
    return kOfxStatReplyDefault;
}

// OFX does not tell the entry point which plugin is called, so each plugin gets its own
// entry point (and its overlay's), which only passes its effect on: one per effect, in the
// order of ms_effects (effects/effects.c: MS_Test, then effects/list.def).
#define ENTRIES(name)                                                                              \
    static OfxStatus entry_##name(const char *action, const void *handle,                          \
                                  OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {   \
        return main_entry(&ms_##name##_effect, action, handle, in_args, out_args);                 \
    }                                                                                              \
    static OfxStatus overlay_##name(const char *action, const void *handle,                        \
                                    OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) { \
        return ms_overlay_main(&ms_##name##_effect, action, handle, in_args, out_args);            \
    }
ENTRIES(test)
#define MS_EFFECT(Name, name, pipl_id, varies) ENTRIES(name)
#include "effects/list.def"
#undef MS_EFFECT

static OfxPluginEntryPoint *const entries[] = {
    entry_test,
#define MS_EFFECT(Name, name, pipl_id, varies) entry_##name,
#include "effects/list.def"
#undef MS_EFFECT
};
static OfxPluginEntryPoint *const overlays[] = {
    overlay_test,
#define MS_EFFECT(Name, name, pipl_id, varies) overlay_##name,
#include "effects/list.def"
#undef MS_EFFECT
};

static OfxPluginEntryPoint *ms_overlay_entry(int index) { return overlays[index]; }

static OfxPlugin plugins[ARRAY_SIZE(entries)];

OfxPlugin *ms_ofx_plugin(int index) {
    if (index < 0 || index >= ms_effect_count || index >= (int)ARRAY_SIZE(entries))
        return NULL;
    plugins[index] = (OfxPlugin){
        kOfxImageEffectPluginApi, 1, ms_effects[index]->id, 1, 0, ms_set_host, entries[index]};
    return &plugins[index];
}
