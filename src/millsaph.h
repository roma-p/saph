// Shared declarations for the MillSaph plugins.

#pragma once

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#ifdef _WIN32
#define MS_EXPORT __declspec(dllexport)
#else
#define MS_EXPORT __attribute__((visibility("default")))
#endif

#define MS_GROUP "MillSaph"

#define MS_ID(name) "com.xiaoluo.millsaph." name

/*
*******************************************************************************

MEMO ON OFX Plugin API:
- describe: 
    Called once, when the host scans plugins.
    It declares name / menu / use tiles etc... 

- describe_in_context:
    declare input / output of the effects

- create_instance:
    start gpu code

- render: 
    compute one frame.

- main_entry: 
    the single entry to holds everything.

*******************************************************************************
*/


// Host suites, fetched once when the plugin is loaded (see millsaph.c).
extern const OfxHost* ms_host;
extern const OfxPropertySuiteV1* ms_prop;
extern const OfxImageEffectSuiteV1* ms_effect;
extern const OfxParameterSuiteV1* ms_param;
extern const OfxMessageSuiteV1* ms_message;

// Called by each plugin's setHost / Load / Unload action.
void ms_set_host(OfxHost* host);
OfxStatus ms_load(void);
OfxStatus ms_unload(void);
