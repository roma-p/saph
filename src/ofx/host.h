// Shared declarations for the MillSaph plugins.

#pragma once

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "common/effect.h"

#ifdef _WIN32
#define MS_EXPORT __declspec(dllexport)
#else
#define MS_EXPORT __attribute__((visibility("default")))
#endif

// The OFX actions the plugin answers (ofx/plugin.c, main_entry):
// - describe: once, when the host scans plugins: name, menu, depths, no tiles...
// - describe in context: the inputs and the Output of the effect.
// - create instance: fills the text parameters (MS_Test's "Runs on", which starts the GPU).
// - is identity: settings that leave the Source as it is (the host skips the render).
// - render: one frame.

// Host suites, fetched once when the plugin is loaded (see ofx/host.c).
extern const OfxPropertySuiteV1 *ms_prop;
extern const OfxImageEffectSuiteV1 *ms_effect;
extern const OfxParameterSuiteV1 *ms_param;
extern const OfxMessageSuiteV1 *ms_message;

// Called by each plugin's setHost / Load / Unload action.
void ms_set_host(OfxHost *host);
OfxStatus ms_load(void);
OfxStatus ms_unload(void);

// The OFX plugin of ms_effects[index] (see ofx/plugin.c).
OfxPlugin *ms_ofx_plugin(int index);
