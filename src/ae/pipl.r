// PiPL resources: how After Effects finds and lists each effect (see plugin.c). Built by
// CMakeLists.txt, through the C preprocessor first (MS_EFFECT), then on Windows Adobe's
// PiPLtool, on macOS Apple's Rez (after the SDK's AE_General.r, which defines the PiPL
// type). One resource per effect of effects/list.def; the version and the flags must equal
// VERSION, OUT_FLAGS (with PF_OutFlag_NON_PARAM_VARY, 0x4, for the effects that change over
// time by themselves: list.def's varies) and OUT_FLAGS_2 in plugin.c.

#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
#define MS_CODE(ENTRY) CodeMacIntel64 { ENTRY }, CodeMacARM64 { ENTRY },
#else
#define MS_CODE(ENTRY) CodeWin64X86 { ENTRY },
#endif

// (one string from tokens: "MS_BandPass", "MillSaph BandPass")
#define MS_STR(...) #__VA_ARGS__
#define MS_STRING(...) MS_STR(__VA_ARGS__)
// global flags without and with PF_OutFlag_NON_PARAM_VARY
#define MS_FLAGS_0 0x02000000
#define MS_FLAGS_1 0x02000004

#define MS_EFFECT(NAME, LOWER, ID, VARIES)                                                         \
resource 'PiPL' (ID) {                                                                             \
    {                                                                                              \
        Kind { AEEffect },                                                                         \
        Name { MS_STRING(MS_##NAME) },                                                             \
        Category { "MillSaph" },                                                                   \
        MS_CODE(MS_STRING(EffectMain_##NAME))                                                      \
        AE_PiPL_Version { 2, 0 },                                                                  \
        AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },                         \
        AE_Effect_Version { 524289 },                                                              \
        AE_Effect_Info_Flags { 0 },                                                                \
        AE_Effect_Global_OutFlags { MS_FLAGS_##VARIES },                                           \
        AE_Effect_Global_OutFlags_2 { 0x08001400 },                                                \
        AE_Effect_Match_Name { MS_STRING(MillSaph NAME) },                                         \
        AE_Reserved_Info { 0 },                                                                    \
        AE_Effect_Support_URL { "" }                                                               \
    }                                                                                              \
};

#include "../effects/list.def"
