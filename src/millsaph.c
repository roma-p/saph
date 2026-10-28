#include "gpu.h"
#include "millsaph.h"

const OfxHost* ms_host;
const OfxPropertySuiteV1* ms_prop;
const OfxImageEffectSuiteV1* ms_effect;
const OfxParameterSuiteV1* ms_param;
const OfxMessageSuiteV1* ms_message;

static int loaded_count;

void ms_set_host(OfxHost* host) { ms_host = host; }

OfxStatus ms_load(void) {
    if (!ms_host) return kOfxStatErrMissingHostFeature;
    ms_prop = ms_host->fetchSuite(ms_host->host, kOfxPropertySuite, 1);
    ms_effect = ms_host->fetchSuite(ms_host->host, kOfxImageEffectSuite, 1);
    ms_param = ms_host->fetchSuite(ms_host->host, kOfxParameterSuite, 1);
    ms_message = ms_host->fetchSuite(ms_host->host, kOfxMessageSuite, 1);  // optional
    if (!ms_prop || !ms_effect || !ms_param) return kOfxStatErrMissingHostFeature;
    ++loaded_count;
    return kOfxStatOK;
}

OfxStatus ms_unload(void) {
    if (loaded_count > 0 && --loaded_count == 0) ms_gpu_shutdown();
    return kOfxStatOK;
}

// No effects yet.
MS_EXPORT int OfxGetNumberOfPlugins(void) { return 0; }

MS_EXPORT OfxPlugin* OfxGetPlugin(int nth) {
    (void)nth;
    return 0;
}
