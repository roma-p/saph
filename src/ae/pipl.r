// PiPL resources: how After Effects finds and lists each effect (see plugin.c). Built by
// CMakeLists.txt, through the C preprocessor first (MS_EFFECT), then on Windows Adobe's
// PiPLtool, on macOS Apple's Rez (after the SDK's AE_General.r, which defines the PiPL
// type). One resource per effect; the version and the flags must equal VERSION, OUT_FLAGS
// and OUT_FLAGS_2 in plugin.c.

#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
#define MS_CODE(ENTRY) CodeMacIntel64 { ENTRY }, CodeMacARM64 { ENTRY },
#else
#define MS_CODE(ENTRY) CodeWin64X86 { ENTRY },
#endif

#define MS_EFFECT(ID, NAME, MATCH_NAME, ENTRY)                                                     \
resource 'PiPL' (ID) {                                                                             \
    {                                                                                              \
        Kind { AEEffect },                                                                         \
        Name { NAME },                                                                             \
        Category { "MillSaph" },                                                                   \
        MS_CODE(ENTRY)                                                                             \
        AE_PiPL_Version { 2, 0 },                                                                  \
        AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },                         \
        AE_Effect_Version { 524289 },                                                              \
        AE_Effect_Info_Flags { 0 },                                                                \
        AE_Effect_Global_OutFlags { 0x02000000 },                                                  \
        AE_Effect_Global_OutFlags_2 { 0x08001400 },                                                \
        AE_Effect_Match_Name { MATCH_NAME },                                                       \
        AE_Reserved_Info { 0 },                                                                    \
        AE_Effect_Support_URL { "" }                                                               \
    }                                                                                              \
};

MS_EFFECT(16000, "MS_BandPass", "MillSaph BandPass", "EffectMain_BandPass")
MS_EFFECT(16001, "MS_EdgeAwareBlur", "MillSaph EdgeAwareBlur", "EffectMain_EdgeAwareBlur")
MS_EFFECT(16002, "MS_Distort", "MillSaph Distort", "EffectMain_Distort")
