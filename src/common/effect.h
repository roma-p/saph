// Host-neutral description of an effect: its inputs, its parameters and its render
// function. Nothing here depends on a plugin API. The OFX adapter (ofx/plugin.c), and
// later others (Adobe), turn this description into the host's own calls.

#pragma once

#include <stddef.h>

#ifdef __cplusplus // (the lens editor, src/editor)
extern "C" {
#endif

#define MS_GROUP "MillSaph"

#define MS_ID(name) "com.xiaoluo.millsaph." name

// A parameter limit that is not one (MsParam min / max). Not INFINITY: some compilers
// (MSVC) do not take it in static tables.
#define MS_UNBOUNDED 1e30

// One input of an effect. The first one is the main input.
typedef struct {
    const char *clip;  // identifier of the input in the host (saved in scripts);
                       // the main input must be "Source"
    const char *label; // name shown to the user on the node
    float outside;     // value used where the input has no pixels
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
    MS_PARAM_STRING, // an editable text (several lines), saved: MsRender.texts
    MS_PARAM_RGB,    // a colour: red here, green and blue in the next two entries
    // Green or blue of the MS_PARAM_RGB before (`def`: its default; no host parameter).
    MS_PARAM_RGB_PART,
    // A position on the image: x here, y in the next entry (MS_PARAM_POINT_Y). `def`: a
    // fraction of the frame width (y: of the frame height, from the bottom); the effect gets
    // pixels of the render, y up.
    MS_PARAM_POINT,
    MS_PARAM_POINT_Y,
    MS_PARAM_BUTTON, // a push button: MsParam.press runs when it is pressed
} MsParamType;

// What a button's press can do with the effect's parameters in the host (the host's own
// thread, while the user waits): read and set them by name (POINT: x, its _y entry: y).
typedef struct MsHost MsHost;
struct MsHost {
    const char *(*get_text)(MsHost *host, const char *name);           // STRING (NULL: none)
    int (*set_text)(MsHost *host, const char *name, const char *text); // 1: done
    double (*get_value)(MsHost *host, const char *name); // DOUBLE, WIDTH, CHOICE, BOOL...
    int (*set_value)(MsHost *host, const char *name, double value);
    void *data; // the host's
    // Optional (NULL: unknown): the project's frame in pixels (POINT values are in pixels of
    // it, presets hold fractions); returns 0 when the host cannot say.
    int (*frame_size)(MsHost *host, double size[2]);
};

// One parameter. Unset fields are 0, so tables use designated initializers.
typedef struct {
    MsParamType type;
    const char *name; // identifier in the host (saved in scripts)
    const char *label;
    const char *hint;
    double def;      // default (CHOICE: index, BOOL: 0 or 1)
    double min, max; // hard limits, none when both are 0; +-MS_UNBOUNDED leaves one side open
    double display_min, display_max; // slider range, host default when both are 0
    const char *const *entries;      // CHOICE: the menu entries
    int entry_count;
    const char *(*text)(void); // TEXT: the line to show
    const char *def_text;      // STRING: the default text
    // BUTTON: what pressing it does; returns 0 with the reason in err (shown by the host)
    int (*press)(MsHost *host, char *err, size_t err_size);
} MsParam;

// Common kinds of parameter, as table entries ([id] = ...): name n, label l, hint h, default d.
// A scale (0 and up, slider to 5):
#define MS_RELATIVE(id, n, l, h, d)                                                                \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = MS_UNBOUNDED,                                                                   \
            .display_max = 5.0}
// a fraction (0 to 1):
#define MS_FRACTION(id, n, l, h, d)                                                                \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = 1.0,                                                                            \
            .display_max = 1.0}
// a distance (a fraction of the frame's width, 0 and up, slider to 1):
#define MS_DISTANCE(id, n, l, h, d)                                                                \
    [id] = {.type = MS_PARAM_WIDTH,                                                                \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = MS_UNBOUNDED,                                                                   \
            .display_max = 1.0}
// a colour, white by default (its green and blue: entries id + 1 and id + 2, n "_green" and
// n "_blue"):
#define MS_COLOUR(id, n, l, h)                                                                     \
    [id] = {.type = MS_PARAM_RGB, .name = (n), .label = (l), .hint = (h), .def = 1.0},             \
    [id + 1] = {.type = MS_PARAM_RGB_PART, .name = n "_green", .def = 1.0},                        \
    [id + 2] = {.type = MS_PARAM_RGB_PART, .name = n "_blue", .def = 1.0}

// The most inputs and parameters of an effect (the hosts' code sizes arrays by them).
#define MS_MAX_CLIPS 8
#define MS_MAX_PARAMS 96

// An input image: RGBA (channels next to each other), bottom row first, in floats or
// 16-bit floats (MsRender.half), `row` of these from the start of one row to the next (4 x
// width when packed, more when the host's rows are longer).
typedef struct {
    const void *pixels; // NULL when the input is not connected
    ptrdiff_t row;
} MsInput;

// 1 when two connected inputs are one image in the host (e.g. the Source also plugged in
// as the Mask): the effects then upload it to the GPU once.
static inline int ms_same_input(const MsInput *a, const MsInput *b) {
    return a->pixels && a->pixels == b->pixels && a->row == b->row;
}

// What an effect's render function receives. Images are width x height.
typedef struct MsRender {
    const double *values;  // one per parameter, in table order; WIDTH and HEIGHT in pixels
    const int *connected;  // one per input: 1 if it has an image
    const MsInput *inputs; // one per input, in table order (MsClip.outside where a connected
                           // input has no pixels)
    int width, height;
    double frame_width, frame_height; // the project frame in pixels of this render (what
                                      // WIDTH and HEIGHT values are fractions of)
    void *output;                     // the result to fill, as an input
    ptrdiff_t output_row;
    char *err; // reason of a failure
    size_t err_size;
    int half;               // the inputs and the output in 16-bit floats (else floats): engine.h,
                            // ms_image_rgba
    struct MsRunInfo *info; // what the effect's ms_run did (engine.h), or NULL
    // One per parameter: the STRING parameters' texts (NULL for the others). NULL when the
    // caller has none to give (the effects then take their default texts).
    const char *const *texts;
    double time; // the frame rendered (the host's time, in frames)
    // Optional (NULL: the caller cannot): the parameters' values at another time (frames, e.g.
    // between two frames for a motion blur) into `values`, as `values` holds them at `time`;
    // returns 0 when it cannot. `caller`: the caller's own.
    int (*values_at)(const struct MsRender *r, double time, double *values);
    void *caller;
} MsRender;

// One shape of an effect's viewer overlay (MsEffect.overlay), and what dragging it edits.
// Parameters are named; NULL leaves one out. Lengths are pixels of the frame. A shape is
// drawn around its centre: the POINT `point` (with `point2`: halfway between the two), or
// the middle of the Source's frame without one. Its length r = factor x size x scale
// (factor 0: 1, scale NULL: 1); rel_x and rel_y stretch it across and up.
typedef enum {
    // An ellipse, radii r x rel_x and r x rel_y. Drags `scale` (else `size`): the ellipse
    // follows the pen.
    MS_OVERLAY_ELLIPSE,
    // A double arrow (a fixed size on the screen) across the ellipse's right end: drags
    // rel_x (the arrow follows the pen).
    MS_OVERLAY_ARROW_X,
    MS_OVERLAY_ARROW_Y, // the same on its top end: drags rel_y
    // A line through the centre, from -r x dir to r x dir: drags `scale` (else `size`), its
    // end follows the pen.
    MS_OVERLAY_RAY,
    MS_OVERLAY_DOT, // a dot on the centre, not dragged
    // A circle with a cross (a fixed size on the screen): drags the centre (`point`, and
    // `point2` with it).
    MS_OVERLAY_CROSSHAIR,
    MS_OVERLAY_SUN, // eight short rays round the centre (fixed size): drags it as CROSSHAIR
    // The rectangle with corners `point` and `point2`: its corners and its sides drag the
    // coordinates of the points they are made of.
    MS_OVERLAY_RECT,
} MsOverlayKind;

typedef struct {
    MsOverlayKind kind;
    float colour[3];
    const char *show; // a BOOL parameter that hides the shape when off
    const char *point, *point2;
    const char *size, *scale, *rel_x, *rel_y;
    double factor;
    double dir[2]; // RAY: its direction (from the centre to the end where r = 1)
} MsOverlay;

typedef struct {
    const char *id;    // MS_ID("Name")
    const char *label; // name shown in the host's menus
    const char *description;
    const MsClip *clips;
    int clip_count;
    const MsParam *params;
    int param_count;
    int (*render)(const MsRender *r); // returns 1 on success, or 0 with the reason in r->err
    // Optional: 1 when these settings leave the Source as it is (hosts then use it without
    // rendering). `values` and `connected` as in MsRender.
    int (*identity)(const double *values, const int *connected);
    // 1 when the result changes from frame to frame with the same settings and inputs (the
    // hosts then render every frame).
    int frame_varying;
    // Optional: frees what the effect keeps from render to render (the plugin is unloaded).
    void (*release)(void);
    // Optional: a viewer overlay, its shapes in drawing order (the hosts' own handles of
    // the POINT parameters are then off: the overlay draws and drags them).
    const MsOverlay *overlay;
    int overlay_count;
    // Optional: the folder of the effect's presets (common/preset.h); the hosts then show a
    // Load Preset button.
    const char *presets;
    // Optional: 1 when the result changes from frame to frame with these settings (values as
    // MsRender.values; frame_varying: always). The hosts ask again when a parameter changes.
    int (*varying)(const double *values);
} MsEffect;

// Shape colours of the viewer overlays.
#define MS_OVERLAY_ORANGE {0.9f, 0.4f, 0.1f}
#define MS_OVERLAY_WHITE {1.0f, 1.0f, 1.0f}
#define MS_OVERLAY_RED {1.0f, 0.0f, 0.0f}
#define MS_OVERLAY_GREEN {0.1f, 0.9f, 0.0f}
#define MS_OVERLAY_BLUE {0.4f, 0.4f, 1.0f}

// A "show" checkbox of an overlay's shapes (on by default), as a table entry.
#define MS_SHOW(id, n, l)                                                                          \
    [id] = {.type = MS_PARAM_BOOL,                                                                 \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = "Draws its controls in the viewer.",                                           \
            .def = 1.0}

// Every effect, in menu order (see effects/effects.c).
extern const MsEffect *const ms_effects[];
extern const int ms_effect_count;

// Calls every effect's release (the plugin is unloaded).
void ms_effects_release(void);

// The effects.
extern const MsEffect ms_test_effect;
#define MS_EFFECT(Name, name, pipl_id, varies) extern const MsEffect ms_##name##_effect;
#include "effects/list.def"
#undef MS_EFFECT

#ifdef __cplusplus
}
#endif
