// MillSaph OFX plugin entry points.

#include "ofxCore.h"

#ifdef _WIN32
#define MS_EXPORT __declspec(dllexport)
#else
#define MS_EXPORT __attribute__((visibility("default")))
#endif

MS_EXPORT int OfxGetNumberOfPlugins(void) { return 0; }

MS_EXPORT OfxPlugin* OfxGetPlugin(int nth) {
    (void)nth;
    return 0;
}
