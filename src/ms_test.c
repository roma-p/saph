// MS_Test: a diagnostic effect, simple gain shader to validate shader scaffolding.
// Its "GPU" line shows which graphics card and driver are used, or why the GPU failed.

#include <string.h>

#include "common/defines.h"
#include "gpu.h"
#include "image.h"
#include "millsaph.h"
#include "ms_test_glsl.h"

#define GAIN "gain"
#define GPU_INFO "gpu"

// Inputs: OFX clip, label in the host, name in the shader, value where there is no
// image, optional, mask.
static const MsClip clips[] = {
    {kOfxImageEffectSimpleSourceClipName, "Source", "source", 0.0f, 0, 0},
};


static OfxStatus describe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    ms_effect->getPropertySet(effect, &props);
    ms_prop->propSetString(props, kOfxPropLabel, 0, "MS_Test");
    ms_prop->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, MS_GROUP);
    ms_prop->propSetString(props, kOfxPropPluginDescription, 0,
                           "MillSaph diagnostic: multiplies the Source by Gain.");
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);

    // Float only: host (dcc) are exepcted to convert to float if needed.
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

    // Whole frames, no tiling.
    ms_prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    ms_prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    return kOfxStatOK;
}

static OfxStatus describe_in_context(OfxImageEffectHandle effect, OfxPropertySetHandle in_args) {
    OfxPropertySetHandle props;
    ms_define_clips(effect, in_args, clips, ARRAY_SIZE(clips));

    OfxParamSetHandle params;
    ms_effect->getParamSet(effect, &params);
    ms_param->paramDefine(params, kOfxParamTypeDouble, GAIN, &props);
    ms_prop->propSetString(props, kOfxPropLabel, 0, "Gain");
    ms_prop->propSetString(props, kOfxParamPropHint, 0, "Output = Source x Gain");
    ms_prop->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
    ms_prop->propSetDouble(props, kOfxParamPropMin, 0, 0.0);
    ms_prop->propSetDouble(props, kOfxParamPropMax, 0, 100.0);
    ms_prop->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0.0);
    ms_prop->propSetDouble(props, kOfxParamPropDisplayMax, 0, 4.0);

    ms_param->paramDefine(params, kOfxParamTypeString, GPU_INFO, &props);
    ms_prop->propSetString(props, kOfxPropLabel, 0, "GPU");
    ms_prop->propSetInt(props, kOfxParamPropEnabled, 0, 0);
    ms_prop->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 0);
    ms_prop->propSetInt(props, kOfxParamPropPersistant, 0, 0);
    return kOfxStatOK;
}

// Shows the GPU in use (this also starts the GPU, so problems show up straight away).
// only overriden for ms test (for the other effect, default create_instance is ok)
static OfxStatus create_instance(OfxImageEffectHandle effect) {
    OfxParamSetHandle params;
    OfxParamHandle info;
    if (ms_effect->getParamSet(effect, &params) == kOfxStatOK &&
        ms_param->paramGetHandle(params, GPU_INFO, &info, NULL) == kOfxStatOK)
        ms_param->paramSetValue(info, ms_gpu_info());
    return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in_args) {
    OfxTime time;
    OfxParamSetHandle params;
    OfxParamHandle gain_param;
    double gain;
    if (ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time) != kOfxStatOK ||
        ms_effect->getParamSet(effect, &params) != kOfxStatOK ||
        ms_param->paramGetHandle(params, GAIN, &gain_param, NULL) != kOfxStatOK ||
        ms_param->paramGetValueAtTime(gain_param, time, &gain) != kOfxStatOK)
        return kOfxStatFailed;

    const MsShaderValue shader_values[] = {{"gain", (float)gain}};
    static const char* const passes[] = {ms_test_glsl};
    return ms_render(effect, in_args, clips, ARRAY_SIZE(clips), passes, ARRAY_SIZE(passes), shader_values,
                     ARRAY_SIZE(shader_values));
}

static OfxStatus main_entry(const char* action, const void* handle, OfxPropertySetHandle in_args,
                            OfxPropertySetHandle out_args) {
    (void)out_args;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (strcmp(action, kOfxActionLoad) == 0) return ms_load();
    if (strcmp(action, kOfxActionUnload) == 0) return ms_unload();
    if (strcmp(action, kOfxActionDescribe) == 0) return describe(effect);
    if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) return describe_in_context(effect, in_args);
    if (strcmp(action, kOfxActionCreateInstance) == 0) return create_instance(effect);
    if (strcmp(action, kOfxImageEffectActionRender) == 0) return render(effect, in_args);
    return kOfxStatReplyDefault;
}

OfxPlugin ms_test_plugin = {
    kOfxImageEffectPluginApi,
    1,
    MS_ID("Test"),
    1, 0,
    ms_set_host,
    main_entry,
};
