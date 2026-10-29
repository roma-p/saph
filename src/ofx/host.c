#include "ofx/host.h"
#include "common/defines.h"
#include "gpu/gpu.h"

static const OfxHost *ofx_host; // given by setHost
const OfxPropertySuiteV1 *ms_prop;
const OfxImageEffectSuiteV1 *ms_effect;
const OfxParameterSuiteV1 *ms_param;
const OfxMessageSuiteV1 *ms_message;

static int loaded_count;

void ms_set_host(OfxHost *host) { ofx_host = host; }

OfxStatus ms_load(void) {
    if (!ofx_host)
        return kOfxStatErrMissingHostFeature;
    ms_prop = ofx_host->fetchSuite(ofx_host->host, kOfxPropertySuite, 1);
    ms_effect = ofx_host->fetchSuite(ofx_host->host, kOfxImageEffectSuite, 1);
    ms_param = ofx_host->fetchSuite(ofx_host->host, kOfxParameterSuite, 1);
    ms_message = ofx_host->fetchSuite(ofx_host->host, kOfxMessageSuite, 1); // optional
    if (!ms_prop || !ms_effect || !ms_param)
        return kOfxStatErrMissingHostFeature;
    ++loaded_count;
    return kOfxStatOK;
}

OfxStatus ms_unload(void) {
    if (loaded_count == 0)
        return kOfxStatOK;
    --loaded_count;
    if (loaded_count == 0)
        ms_gpu_shutdown();
    return kOfxStatOK;
}

MS_EXPORT int OfxGetNumberOfPlugins(void) { return ms_effect_count; }

MS_EXPORT OfxPlugin *OfxGetPlugin(int nth) { return ms_ofx_plugin(nth); }
