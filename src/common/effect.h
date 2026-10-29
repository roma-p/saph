// Host-neutral description of an effect: its inputs, its parameters and its render
// function. Nothing here depends on a plugin API. The OFX adapter (ofx/plugin.c), and
// later others (Adobe), turn this description into the host's own calls.

#pragma once

#include <stddef.h>

#include "gpu/gpu.h"

#define MS_GROUP "MillSaph"

#define MS_ID(name) "com.xiaoluo.millsaph." name

// One input of an effect. The first one is the main input.
typedef struct {
    const char *clip;  // identifier of the input in the host (saved in scripts);
                       // the main input must be "Source"
    const char *label; // name shown to the user on the node
    const char *name;  // name the shaders read it by (`uniform sampler2D <name>`)
    float outside;     // value used where the input has no pixels (e.g. not connected)
    int optional;      // the effect works without it
    int mask;          // hosts show it as a mask input
} MsClip;

typedef enum {
    MS_PARAM_DOUBLE, // a slider
    MS_PARAM_WIDTH,  // a fraction of the frame width (OFX "normalised X"):
                     // hosts show it in pixels, the effect gets it in pixels
    MS_PARAM_HEIGHT, // same with the frame height
    MS_PARAM_ANGLE,  // degrees
    MS_PARAM_CHOICE, // a popup menu: the value is the index of the chosen entry
    MS_PARAM_BOOL,   // a checkbox: 0 or 1
    MS_PARAM_TEXT,   // a read-only line of text, set by `text` when the effect is created
} MsParamType;

// One parameter. Unset fields are 0, so tables use designated initializers.
typedef struct {
    MsParamType type;
    const char *name; // identifier in the host (saved in scripts)
    const char *label;
    const char *hint;
    double def;      // default (CHOICE: index, BOOL: 0 or 1)
    double min, max; // hard limits, none when both are 0; +-INFINITY leaves one side open
    double display_min, display_max; // slider range, host default when both are 0
    const char *const *entries;      // CHOICE: the menu entries
    int entry_count;
    const char *(*text)(void); // TEXT: the line to show
} MsParam;

// What an effect's render function receives. Images are packed RGBA floats, width x
// height, bottom row first.
typedef struct {
    const double *values;  // one per parameter, in table order; WIDTH and HEIGHT in pixels
    const int *connected;  // one per input: 1 if it has an image
    const MsInput *inputs; // one per input, in table order (MsClip.outside where it has no pixels)
    int width, height;
    double frame_width, frame_height; // the project frame in pixels of this render (what
                                      // WIDTH and HEIGHT values are fractions of)
    float *output;                    // the result to fill
    char *err;                        // reason of a failure
    size_t err_size;
} MsRender;

typedef struct {
    const char *id;    // MS_ID("Name")
    const char *label; // name shown in the host's menus
    const char *description;
    const MsClip *clips;
    int clip_count;
    const MsParam *params;
    int param_count;
    int (*render)(const MsRender *r); // returns 1 on success, or 0 with the reason in r->err
} MsEffect;

// Every effect, in menu order (see effects/effects.c).
extern const MsEffect *const ms_effects[];
extern const int ms_effect_count;

// The effects.
extern const MsEffect ms_test_effect;
extern const MsEffect ms_bandpass_effect;
extern const MsEffect ms_edgeawareblur_effect;
extern const MsEffect ms_distort_effect;
