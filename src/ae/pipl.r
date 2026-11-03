// PiPL resources: how After Effects finds and lists each effect (see plugin.c). Built by
// CMakeLists.txt with Adobe's PiPLtool. One resource per effect; the version and the
// flags must equal VERSION, OUT_FLAGS and OUT_FLAGS_2 in plugin.c.

#include "AEConfig.h"
#include "AE_EffectVers.h"

#define MS_EFFECT(ID, NAME, MATCH_NAME, ENTRY)                                                     \
resource 'PiPL' (ID) {                                                                             \
    {                                                                                              \
        Kind { AEEffect },                                                                         \
        Name { NAME },                                                                             \
        Category { "MillSaph" },                                                                   \
        CodeWin64X86 { ENTRY },                                                                    \
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
