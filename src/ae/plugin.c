// After Effects adapter: makes an After Effects effect of each MillSaph effect (but MS_Test;
// common/effect.h). SmartFX, 8, 16 and 32 bits per channel. Windows and macOS (see
// CMakeLists.txt: it needs Adobe's After Effects SDK).
//
// Each effect has its own entry point (EffectMain_<Name>) and PiPL resource (pipl.r).
// Its After Effects parameters, by index:
//   0             the effect's own layer: the Source
//   1 ...         the effect's parameters, in table order (TEXT ones are left out; an RGB
//                 colour is one parameter; a STRING is arbitrary data with no control of its
//                 own, its text saved in the project; a BUTTON is a button)
//   then          one layer menu per other input (Mask, Lens...), "None" by default
// WIDTH and HEIGHT parameters are shown as a percentage of the layer width / height.
//
// The whole layer is computed at once (no tiles), as with OFX.
//
// Not yet tried in After Effects. Open points to check there:
// - alpha: the effects get the pixels as After Effects gives them (it may be straight,
//   unpremultiplied, alpha, where OFX hosts give premultiplied images);
// - origin_x / origin_y of the checked-out buffers, read here as the position of their
//   top-left pixel in the layer;
// - the STRING parameters as arbitrary data with no control (PF_PUI_NO_ECW_UI), changed by a
//   button's press through the params array of PF_Cmd_USER_CHANGED_PARAM (the handle resized
//   in place);
// - the parameters read between frames (MsRender.values_at: a motion blur), in TIME_STEPS
//   units a frame.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectSuites.h"
#include "AE_PluginData.h"

#include "common/defines.h"
#include "common/effect.h"
#include "engine/engine.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

// These must match AE_Effect_Version and AE_Effect_Global_OutFlags(_2) in pipl.r, or
// After Effects warns when it loads the plugin. The effects that change over time by
// themselves (MsEffect.frame_varying) add PF_OutFlag_NON_PARAM_VARY (0x4).
#define VERSION PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 1) // 524289
#define OUT_FLAGS PF_OutFlag_DEEP_COLOR_AWARE            // 0x02000000
#define OUT_FLAGS_2                                                                                \
    (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |                           \
     PF_OutFlag2_SUPPORTS_THREADED_RENDERING) // 0x08001400

#define PROBE_ID 100      // checkout id of the first look at the Source (see pre_render)
#define SLIDER_LIMIT 1e6f // After Effects sliders need finite limits

typedef struct {
    const MsEffect *effect;
    char popups[MS_MAX_PARAMS][256]; // CHOICE entries as After Effects wants them: "A|B|C"
} AeEffect;

// every effect of effects/list.def (not MS_Test)
static AeEffect effects[] = {
#define MS_EFFECT(Name, name, pipl_id, varies) {&ms_##name##_effect, {{0}}},
#include "effects/list.def"
#undef MS_EFFECT
};
enum { // their slots
#define MS_EFFECT(Name, name, pipl_id, varies) SLOT_##Name,
#include "effects/list.def"
#undef MS_EFFECT
};

// ---------------------------------------------------------------- parameters

// 1 when parameter type `t` has an After Effects parameter (TEXT ones are left out, and the
// green and blue of an RGB are in its colour).
static int in_ae(MsParamType t) {
    return t != MS_PARAM_TEXT && t != MS_PARAM_RGB_PART && t != MS_PARAM_POINT_Y;
}

// The effect parameter of After Effects index `index` (-1: none).
static int param_at(const MsEffect *e, int index) {
    for (int i = 0, k = 1; i < e->param_count; ++i)
        if (in_ae(e->params[i].type) && k++ == index)
            return i;
    return -1;
}

// The effect parameter named `name` (-1: none).
static int param_named(const MsEffect *e, const char *name) {
    for (int i = 0; i < e->param_count; ++i)
        if (e->params[i].name && strcmp(e->params[i].name, name) == 0)
            return i;
    return -1;
}

// After Effects index of effect parameter i (0 for one left out).
static int param_index(const MsEffect *e, int i) {
    if (!in_ae(e->params[i].type))
        return 0;
    int index = 1;
    for (int k = 0; k < i; ++k)
        if (in_ae(e->params[k].type))
            ++index;
    return index;
}

// After Effects index of input c (c >= 1: input 0 is the effect's own layer).
static int layer_index(const MsEffect *e, int c) {
    int index = 1;
    for (int k = 0; k < e->param_count; ++k)
        if (in_ae(e->params[k].type))
            ++index;
    return index + c - 1;
}

static A_u_char to_byte(double v) { return (A_u_char)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5); }

static PF_Err add_param(PF_InData *in_data, PF_ParamDef *def) {
    return in_data->inter.add_param(in_data->effect_ref, -1, def);
}

// ---------------------------------------------------------------- texts (STRING)

// A STRING parameter's text: arbitrary data, a handle holding the text and its 0.
static PF_Handle new_text(PF_InData *in_data, const char *text, size_t length) {
    PF_Handle h = in_data->utils->host_new_handle(length + 1);
    char *p = h ? in_data->utils->host_lock_handle(h) : NULL;
    if (p) {
        memcpy(p, text, length);
        p[length] = 0;
        in_data->utils->host_unlock_handle(h);
    }
    return h;
}

// A copy of a text handle's text (malloc; "" for none), to free.
static char *text_of(PF_InData *in_data, PF_Handle h) {
    const char *p = h ? in_data->utils->host_lock_handle(h) : NULL;
    const size_t size = h ? (size_t)in_data->utils->host_get_handle_size(h) : 0;
    char *copy = malloc(size + 1);
    if (copy) {
        if (p && size)
            memcpy(copy, p, size);
        copy[size] = 0;
    }
    if (p)
        in_data->utils->host_unlock_handle(h);
    return copy;
}

// The arbitrary data callbacks of the STRING parameters (their id: the After Effects index).
static PF_Err text_callback(const MsEffect *e, PF_InData *in_data, PF_ArbParamsExtra *x) {
    char *a = NULL, *b = NULL;
    PF_Err err = PF_Err_NONE;
    switch (x->which_function) {
    case PF_Arbitrary_NEW_FUNC: {
        const int i = param_at(e, x->id);
        const char *text = i >= 0 && e->params[i].def_text ? e->params[i].def_text : "";
        *x->u.new_func_params.arbPH = new_text(in_data, text, strlen(text));
        break;
    }
    case PF_Arbitrary_DISPOSE_FUNC:
        if (x->u.dispose_func_params.arbH)
            in_data->utils->host_dispose_handle(x->u.dispose_func_params.arbH);
        break;
    case PF_Arbitrary_COPY_FUNC:
        a = text_of(in_data, x->u.copy_func_params.src_arbH);
        *x->u.copy_func_params.dst_arbPH = a ? new_text(in_data, a, strlen(a)) : NULL;
        break;
    case PF_Arbitrary_FLAT_SIZE_FUNC:
        a = text_of(in_data, x->u.flat_size_func_params.arbH);
        *x->u.flat_size_func_params.flat_data_sizePLu = a ? (A_u_long)strlen(a) + 1 : 1;
        break;
    case PF_Arbitrary_FLATTEN_FUNC:
        a = text_of(in_data, x->u.flatten_func_params.arbH);
        if (a && x->u.flatten_func_params.buf_sizeLu > 0) {
            const size_t n = strlen(a) + 1 < x->u.flatten_func_params.buf_sizeLu
                                 ? strlen(a) + 1
                                 : x->u.flatten_func_params.buf_sizeLu;
            memcpy(x->u.flatten_func_params.flat_dataPV, a, n);
            ((char *)x->u.flatten_func_params.flat_dataPV)[n - 1] = 0;
        }
        break;
    case PF_Arbitrary_UNFLATTEN_FUNC: {
        const char *flat = x->u.unflatten_func_params.flat_dataPV;
        size_t n = 0;
        while (n < x->u.unflatten_func_params.buf_sizeLu && flat[n])
            ++n;
        *x->u.unflatten_func_params.arbPH = new_text(in_data, flat, n);
        break;
    }
    case PF_Arbitrary_INTERP_FUNC: // (no animation: the left key's)
        a = text_of(in_data, x->u.interp_func_params.left_arbH);
        *x->u.interp_func_params.interpPH = a ? new_text(in_data, a, strlen(a)) : NULL;
        break;
    case PF_Arbitrary_COMPARE_FUNC:
        a = text_of(in_data, x->u.compare_func_params.a_arbH);
        b = text_of(in_data, x->u.compare_func_params.b_arbH);
        *x->u.compare_func_params.compareP =
            a && b && strcmp(a, b) == 0 ? PF_ArbCompare_EQUAL : PF_ArbCompare_NOT_EQUAL;
        break;
    case PF_Arbitrary_PRINT_SIZE_FUNC:
        a = text_of(in_data, x->u.print_size_func_params.arbH);
        *x->u.print_size_func_params.print_sizePLu = a ? (A_u_long)strlen(a) + 1 : 1;
        break;
    case PF_Arbitrary_PRINT_FUNC:
        a = text_of(in_data, x->u.print_func_params.arbH);
        if (a && x->u.print_func_params.print_sizeLu > 0)
            snprintf(x->u.print_func_params.print_bufferPC, x->u.print_func_params.print_sizeLu,
                     "%s", a);
        break;
    case PF_Arbitrary_SCAN_FUNC:
        *x->u.scan_func_params.arbPH =
            new_text(in_data, x->u.scan_func_params.bufPC, x->u.scan_func_params.bytes_to_scanLu);
        break;
    default:
        break;
    }
    free(a);
    free(b);
    return err;
}

static PF_Err params_setup(AeEffect *ae, PF_InData *in_data, PF_OutData *out_data) {
    const MsEffect *e = ae->effect;
    if (e->param_count > MS_MAX_PARAMS || e->clip_count > MS_MAX_CLIPS)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    PF_Err err = PF_Err_NONE;
    for (int i = 0; i < e->param_count && !err; ++i) {
        const MsParam *p = &e->params[i];
        if (!in_ae(p->type))
            continue;
        PF_ParamDef def;
        memset(&def, 0, sizeof def);
        snprintf(def.PF_DEF_NAME, sizeof def.PF_DEF_NAME, "%s", p->label);
        def.uu.id = param_index(e, i); // saved in projects: keep it stable

        if (p->type == MS_PARAM_STRING) {
            // arbitrary data: no control (Edit Lens and the like change it), saved, not
            // animated
            def.param_type = PF_Param_ARBITRARY_DATA;
            def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
            def.ui_flags = PF_PUI_NO_ECW_UI;
            const char *text = p->def_text ? p->def_text : "";
            def.u.arb_d.id = (A_short)def.uu.id;
            def.u.arb_d.dephault = new_text(in_data, text, strlen(text));
            def.u.arb_d.value = NULL;
        } else if (p->type == MS_PARAM_BUTTON) {
            def.param_type = PF_Param_BUTTON;
            def.flags = PF_ParamFlag_SUPERVISE;
            def.u.button_d.u.namesptr = p->label;
        } else if (p->type == MS_PARAM_CHOICE) {
            char *text = ae->popups[i];
            text[0] = 0;
            for (int k = 0; k < p->entry_count; ++k)
                snprintf(text + strlen(text), sizeof ae->popups[i] - strlen(text), "%s%s",
                         k ? "|" : "", p->entries[k]);
            def.param_type = PF_Param_POPUP;
            def.u.pd.num_choices = (A_short)p->entry_count;
            def.u.pd.dephault = def.u.pd.value = (A_short)(p->def + 1); // 1 = first entry
            def.u.pd.u.namesptr = text;
        } else if (p->type == MS_PARAM_BOOL) {
            def.param_type = PF_Param_CHECKBOX;
            def.u.bd.dephault = def.u.bd.value = p->def != 0.0;
            def.u.bd.u.nameptr = "";
        } else if (p->type == MS_PARAM_ANGLE) {
            def.param_type = PF_Param_ANGLE;
            def.u.ad.dephault = def.u.ad.value = (PF_Fixed)(p->def * 65536.0); // degrees, 16.16
        } else if (p->type == MS_PARAM_POINT) { // x here, y in the next entry
            // percentages of the layer, from its top-left corner
            def.param_type = PF_Param_POINT;
            def.u.td.x_dephault = (PF_Fixed)(p[0].def * 100.0 * 65536.0);
            def.u.td.y_dephault = (PF_Fixed)((1.0 - p[1].def) * 100.0 * 65536.0);
        } else if (p->type == MS_PARAM_RGB) { // red here, green and blue in the next two
            def.param_type = PF_Param_COLOR;
            def.u.cd.dephault =
                (PF_Pixel){255, to_byte(p[0].def), to_byte(p[1].def), to_byte(p[2].def)};
            def.u.cd.value = def.u.cd.dephault;
        } else {
            // DOUBLE, WIDTH, HEIGHT: a slider. WIDTH and HEIGHT in % of the layer.
            const int percent = p->type == MS_PARAM_WIDTH || p->type == MS_PARAM_HEIGHT;
            const double k = percent ? 100.0 : 1.0;
            const int limited = p->min != 0.0 || p->max != 0.0;
            float lo = limited && p->min > -MS_UNBOUNDED ? (float)(p->min * k) : -SLIDER_LIMIT;
            float hi = limited && p->max < MS_UNBOUNDED ? (float)(p->max * k) : SLIDER_LIMIT;
            const int shown = p->display_min != 0.0 || p->display_max != 0.0;
            def.param_type = PF_Param_FLOAT_SLIDER;
            def.u.fs_d.valid_min = lo;
            def.u.fs_d.valid_max = hi;
            def.u.fs_d.slider_min = shown ? (float)(p->display_min * k) : lo;
            def.u.fs_d.slider_max = shown ? (float)(p->display_max * k) : hi;
            def.u.fs_d.value = p->def * k;
            def.u.fs_d.dephault = (PF_FpShort)(p->def * k);
            def.u.fs_d.precision = percent ? 2 : 3;
            def.u.fs_d.display_flags =
                percent ? PF_ValueDisplayFlag_PERCENT : PF_ValueDisplayFlag_NONE;
        }
        err = add_param(in_data, &def);
    }
    for (int c = 1; c < e->clip_count && !err; ++c) {
        PF_ParamDef def;
        memset(&def, 0, sizeof def);
        snprintf(def.PF_DEF_NAME, sizeof def.PF_DEF_NAME, "%s", e->clips[c].label);
        def.param_type = PF_Param_LAYER;
        def.u.ld.dephault = PF_LayerDefault_NONE;
        def.uu.id = layer_index(e, c);
        err = add_param(in_data, &def);
    }
    out_data->num_params = layer_index(e, e->clip_count); // with the Source layer
    return err;
}

// A colour parameter's value in floats (the colour suite: in the effect's working space,
// over and under range possible), else from its 8-bit value.
static void read_color(PF_InData *in_data, const PF_ParamDef *def, double *rgb) {
    const PF_ColorParamSuite1 *suite = NULL;
    PF_PixelFloat c;
    if (in_data->pica_basicP &&
        in_data->pica_basicP->AcquireSuite(kPFColorParamSuite, kPFColorParamSuiteVersion1,
                                           (const void **)&suite) == kSPNoError &&
        suite) {
        const PF_Err err =
            suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, def, &c);
        in_data->pica_basicP->ReleaseSuite(kPFColorParamSuite, kPFColorParamSuiteVersion1);
        if (!err) {
            rgb[0] = c.red, rgb[1] = c.green, rgb[2] = c.blue;
            return;
        }
    }
    rgb[0] = def->u.cd.value.red / 255.0;
    rgb[1] = def->u.cd.value.green / 255.0;
    rgb[2] = def->u.cd.value.blue / 255.0;
}

// Times between frames: After Effects times are integers (time_scale units a second), so
// these are read in finer units, TIME_STEPS a frame.
#define TIME_STEPS 64

// The parameter values at `frame` (frames, maybe between two), as the effect wants them (see
// MsRender); the STRING ones' texts into `texts` (malloc'd, to free), or left out if NULL.
static PF_Err read_values(const MsEffect *e, PF_InData *in_data, double frame, double frame_width,
                          double frame_height, double *values, char **texts) {
    const A_long step = in_data->time_step * TIME_STEPS;
    const A_long time = (A_long)(frame * step + (frame < 0 ? -0.5 : 0.5));
    const A_u_long scale = in_data->time_scale * TIME_STEPS;
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type != MS_PARAM_RGB_PART && p->type != MS_PARAM_POINT_Y) // with the one before
            values[i] = 0.0;
        if (texts)
            texts[i] = NULL;
        if (!in_ae(p->type) || p->type == MS_PARAM_BUTTON || (p->type == MS_PARAM_STRING && !texts))
            continue;
        PF_ParamDef def;
        memset(&def, 0, sizeof def);
        const PF_Err err = in_data->inter.checkout_param(in_data->effect_ref, param_index(e, i),
                                                         time, step, scale, &def);
        if (err)
            return err;
        switch (p->type) {
        case MS_PARAM_STRING:
            texts[i] = text_of(in_data, def.u.arb_d.value);
            break;
        case MS_PARAM_CHOICE:
            values[i] = def.u.pd.value - 1;
            break;
        case MS_PARAM_BOOL:
            values[i] = def.u.bd.value != 0;
            break;
        case MS_PARAM_ANGLE:
            values[i] = def.u.ad.value / 65536.0;
            break;
        case MS_PARAM_RGB:
            read_color(in_data, &def, &values[i]);
            break;
        case MS_PARAM_POINT: // layer pixels from the top-left corner, at full size
            values[i] = def.u.td.x_value / 65536.0 * frame_width / in_data->width;
            values[i + 1] =
                frame_height - def.u.td.y_value / 65536.0 * frame_height / in_data->height;
            break;
        case MS_PARAM_WIDTH:
            values[i] = def.u.fs_d.value / 100.0 * frame_width;
            break;
        case MS_PARAM_HEIGHT:
            values[i] = def.u.fs_d.value / 100.0 * frame_height;
            break;
        default:
            values[i] = def.u.fs_d.value;
            break;
        }
        in_data->inter.checkin_param(in_data->effect_ref, &def);
    }
    return PF_Err_NONE;
}

// ---------------------------------------------------------------- images

// A rectangle of the layer, in pixels of this render (top-down, as After Effects).
typedef struct {
    int x, y, width, height;
} Area;

// The layer pixel at (x, y) of `world`, as RGBA floats; 0 if outside it. World pixel
// (0, 0) is at layer position (origin_x, origin_y).
static int read_pixel(const PF_EffectWorld *world, int depth, int x, int y, float rgba[4]) {
    x -= world->origin_x;
    y -= world->origin_y;
    if (x < 0 || y < 0 || x >= world->width || y >= world->height)
        return 0;
    const char *row = (const char *)world->data + (ptrdiff_t)y * world->rowbytes;
    if (depth == 32) {
        const PF_PixelFloat *p = (const PF_PixelFloat *)row + x;
        rgba[0] = p->red, rgba[1] = p->green, rgba[2] = p->blue, rgba[3] = p->alpha;
    } else if (depth == 16) {
        const PF_Pixel16 *p = (const PF_Pixel16 *)row + x;
        const float k = 1.0f / PF_MAX_CHAN16;
        rgba[0] = p->red * k, rgba[1] = p->green * k, rgba[2] = p->blue * k, rgba[3] = p->alpha * k;
    } else {
        const PF_Pixel8 *p = (const PF_Pixel8 *)row + x;
        const float k = 1.0f / PF_MAX_CHAN8;
        rgba[0] = p->red * k, rgba[1] = p->green * k, rgba[2] = p->blue * k, rgba[3] = p->alpha * k;
    }
    return 1;
}

// Fills `buffer` (packed RGBA floats, bottom row first, as the effects want them) with
// the pixels of `world` covering `area`; `outside` where `world` has none (or is NULL).
static void pack(const PF_EffectWorld *world, int depth, Area area, float outside, float *buffer) {
    for (int row = 0; row < area.height; ++row) {
        const int y = area.y + area.height - 1 - row; // bottom row first
        float *out = buffer + (size_t)row * area.width * 4;
        for (int x = 0; x < area.width; ++x, out += 4)
            if (!world || !read_pixel(world, depth, area.x + x, y, out))
                out[0] = out[1] = out[2] = out[3] = outside;
    }
}

static A_u_short to_16(float v) {
    v = v * PF_MAX_CHAN16 + 0.5f;
    return (A_u_short)(v < 0.0f ? 0.0f : (v > PF_MAX_CHAN16 ? PF_MAX_CHAN16 : v));
}

static A_u_char to_8(float v) {
    v = v * PF_MAX_CHAN8 + 0.5f;
    return (A_u_char)(v < 0.0f ? 0.0f : (v > PF_MAX_CHAN8 ? PF_MAX_CHAN8 : v));
}

// Copies the part of `buffer` (covering `area`) that falls in `world` into `world`.
static void unpack(const float *buffer, Area area, PF_EffectWorld *world, int depth) {
    for (int wy = 0; wy < world->height; ++wy) {
        const int y = world->origin_y + wy;
        char *row = (char *)world->data + (ptrdiff_t)wy * world->rowbytes;
        for (int wx = 0; wx < world->width; ++wx) {
            const int x = world->origin_x + wx;
            if (x < area.x || y < area.y || x >= area.x + area.width || y >= area.y + area.height)
                continue;
            const int brow = area.height - 1 - (y - area.y); // bottom row first
            const float *v = buffer + ((size_t)brow * area.width + (x - area.x)) * 4;
            if (depth == 32) {
                PF_PixelFloat *p = (PF_PixelFloat *)row + wx;
                p->red = v[0], p->green = v[1], p->blue = v[2], p->alpha = v[3];
            } else if (depth == 16) {
                PF_Pixel16 *p = (PF_Pixel16 *)row + wx;
                p->red = to_16(v[0]), p->green = to_16(v[1]), p->blue = to_16(v[2]),
                p->alpha = to_16(v[3]);
            } else {
                PF_Pixel8 *p = (PF_Pixel8 *)row + wx;
                p->red = to_8(v[0]), p->green = to_8(v[1]), p->blue = to_8(v[2]),
                p->alpha = to_8(v[3]);
            }
        }
    }
}

// ---------------------------------------------------------------- render (SmartFX)

// Asks for every input whole: the Source layer as far as it goes, the other layers too.
// The result covers the whole Source, even when After Effects asked for less.
static PF_Err pre_render(const MsEffect *e, PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_RenderRequest request = extra->input->output_request;
    request.preserve_rgb_of_zero_alpha = 1;
    PF_CheckoutResult source;
    // First look: how far the Source goes (max_result_rect); then ask for all of it.
    PF_Err err =
        extra->cb->checkout_layer(in_data->effect_ref, 0, PROBE_ID, &request, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &source);
    if (err)
        return err;
    request.rect = source.max_result_rect;
    err = extra->cb->checkout_layer(in_data->effect_ref, 0, 0, &request, in_data->current_time,
                                    in_data->time_step, in_data->time_scale, &source);
    for (int c = 1; c < e->clip_count && !err; ++c) {
        PF_CheckoutResult layer;
        err = extra->cb->checkout_layer(in_data->effect_ref, layer_index(e, c), c, &request,
                                        in_data->current_time, in_data->time_step,
                                        in_data->time_scale, &layer);
    }
    extra->output->result_rect = source.max_result_rect;
    extra->output->max_result_rect = source.max_result_rect;
    extra->output->flags |= PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    return err;
}

// MsRender.values_at: the values at another time (the STRING ones left out).
typedef struct {
    const MsEffect *e;
    PF_InData *in_data;
    double frame_width, frame_height;
} RenderCaller;

static int values_at(const MsRender *r, double time, double *values) {
    const RenderCaller *c = r->caller;
    return read_values(c->e, c->in_data, time, c->frame_width, c->frame_height, values, NULL) ==
           PF_Err_NONE;
}

// Renders the effect on the checked-out layers into `output`.
static PF_Err render(const MsEffect *e, PF_InData *in_data, PF_OutData *out_data, int depth,
                     PF_EffectWorld *const *worlds, PF_EffectWorld *output) {
    // Computed at the size of the Source buffer, in pixels of this render. depth: 8, 16 or
    // 32 bits per channel.
    const Area area = {worlds[0]->origin_x, worlds[0]->origin_y, worlds[0]->width,
                       worlds[0]->height};
    const double frame_width =
        in_data->width * (double)in_data->downsample_x.num / in_data->downsample_x.den;
    const double frame_height =
        in_data->height * (double)in_data->downsample_y.num / in_data->downsample_y.den;
    const size_t bytes = (size_t)area.width * area.height * 4 * sizeof(float);

    double values[MS_MAX_PARAMS];
    char *texts[MS_MAX_PARAMS] = {NULL};
    int connected[MS_MAX_CLIPS];
    MsInput inputs[MS_MAX_CLIPS];
    float *buffers[MS_MAX_CLIPS] = {NULL};
    float *result = malloc(bytes);
    char message[256] = "";
    PF_Err err = PF_Err_NONE;
    int ok = result != NULL;
    for (int c = 0; c < e->clip_count && ok; ++c) {
        buffers[c] = malloc(bytes);
        ok = buffers[c] != NULL;
        if (ok) {
            // A layer menu set to "None" gives no pixels.
            connected[c] = worlds[c] != NULL && worlds[c]->data != NULL;
            pack(connected[c] ? worlds[c] : NULL, depth, area, e->clips[c].outside, buffers[c]);
            inputs[c] = (MsInput){buffers[c], (ptrdiff_t)area.width * 4};
        }
    }
    // The time in frames (AE: in time_scale units, a frame time_step long).
    const double frame =
        in_data->time_step ? (double)in_data->current_time / in_data->time_step : 0.0;
    if (ok)
        err = read_values(e, in_data, frame, frame_width, frame_height, values, texts);
    if (ok && !err) {
        RenderCaller caller = {e, in_data, frame_width, frame_height};
        const MsRender r = {values,
                            connected,
                            inputs,
                            area.width,
                            area.height,
                            frame_width,
                            frame_height,
                            result,
                            (ptrdiff_t)area.width * 4,
                            message,
                            sizeof message,
                            0,
                            NULL,
                            (const char *const *)texts,
                            frame,
                            values_at,
                            &caller};
        if (e->render(&r)) {
            unpack(result, area, output, depth);
        } else {
            snprintf(out_data->return_msg, sizeof out_data->return_msg, "%s: %s", e->label,
                     message);
            out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
        }
    } else if (!ok) {
        err = PF_Err_OUT_OF_MEMORY;
    }
    for (int c = 0; c < e->clip_count; ++c)
        free(buffers[c]);
    for (int i = 0; i < e->param_count; ++i)
        free(texts[i]);
    free(result);
    return err;
}

static PF_Err smart_render(const MsEffect *e, PF_InData *in_data, PF_OutData *out_data,
                           PF_SmartRenderExtra *extra) {
    PF_EffectWorld *worlds[MS_MAX_CLIPS] = {NULL};
    PF_EffectWorld *output = NULL;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, 0, &worlds[0]);
    for (int c = 1; c < e->clip_count && !err; ++c)
        err = extra->cb->checkout_layer_pixels(in_data->effect_ref, c, &worlds[c]);
    if (!err)
        err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && worlds[0] && output)
        err = render(e, in_data, out_data, extra->input->bitdepth, worlds, output);
    for (int c = 0; c < e->clip_count; ++c)
        extra->cb->checkin_layer_pixels(in_data->effect_ref, c);
    return err;
}

// ---------------------------------------------------------------- buttons

// MsHost on the parameters After Effects gives with PF_Cmd_USER_CHANGED_PARAM (`params`, by
// After Effects index): what a button's press reads and sets.
typedef struct {
    const MsEffect *e;
    PF_InData *in_data;
    PF_ParamDef **params;
    char *text; // the last text given by get_text
} AeHost;

static PF_ParamDef *host_param(MsHost *host, const char *name, int *i) {
    const AeHost *h = host->data;
    *i = param_named(h->e, name);
    return *i >= 0 && in_ae(h->e->params[*i].type) ? h->params[param_index(h->e, *i)] : NULL;
}

static const char *host_get_text(MsHost *host, const char *name) {
    AeHost *h = host->data;
    int i;
    PF_ParamDef *def = host_param(host, name, &i);
    if (!def || h->e->params[i].type != MS_PARAM_STRING)
        return NULL;
    free(h->text);
    h->text = text_of(h->in_data, def->u.arb_d.value);
    return h->text;
}

static int host_set_text(MsHost *host, const char *name, const char *text) {
    AeHost *h = host->data;
    int i;
    PF_ParamDef *def = host_param(host, name, &i);
    if (!def || h->e->params[i].type != MS_PARAM_STRING)
        return 0;
    const size_t n = strlen(text);
    PF_Handle handle = def->u.arb_d.value;
    if (!handle || h->in_data->utils->host_resize_handle(n + 1, &handle) != PF_Err_NONE)
        return 0;
    char *p = h->in_data->utils->host_lock_handle(handle);
    if (!p)
        return 0;
    memcpy(p, text, n + 1);
    h->in_data->utils->host_unlock_handle(handle);
    def->u.arb_d.value = handle;
    def->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    return 1;
}

static double host_get_value(MsHost *host, const char *name) {
    int i;
    PF_ParamDef *def = host_param(host, name, &i);
    if (!def)
        return 0.0;
    switch (((AeHost *)host->data)->e->params[i].type) {
    case MS_PARAM_CHOICE:
        return def->u.pd.value - 1;
    case MS_PARAM_BOOL:
        return def->u.bd.value != 0;
    case MS_PARAM_DOUBLE:
        return def->u.fs_d.value;
    default:
        return 0.0;
    }
}

static int host_set_value(MsHost *host, const char *name, double value) {
    int i;
    PF_ParamDef *def = host_param(host, name, &i);
    if (!def)
        return 0;
    switch (((AeHost *)host->data)->e->params[i].type) {
    case MS_PARAM_CHOICE:
        def->u.pd.value = (A_long)value + 1;
        break;
    case MS_PARAM_BOOL:
        def->u.bd.value = value != 0.0;
        break;
    case MS_PARAM_DOUBLE:
        def->u.fs_d.value = value;
        break;
    default:
        return 0;
    }
    def->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    return 1;
}

// A button pressed: its press, on these parameters; its error shown.
static PF_Err user_changed_param(const MsEffect *e, PF_InData *in_data, PF_OutData *out_data,
                                 PF_ParamDef *params[], const PF_UserChangedParamExtra *extra) {
    const int i = extra ? param_at(e, extra->param_index) : -1;
    if (i < 0 || e->params[i].type != MS_PARAM_BUTTON || !e->params[i].press || !params)
        return PF_Err_NONE;
    AeHost data = {e, in_data, params, NULL};
    MsHost host = {host_get_text,  host_set_text, host_get_value,
                   host_set_value, &data,         NULL}; // (the frame's size: unknown)
    char message[256] = "";
    if (!e->params[i].press(&host, message, sizeof message)) {
        snprintf(out_data->return_msg, sizeof out_data->return_msg, "%s: %s", e->label, message);
        out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
    }
    free(data.text);
    return PF_Err_NONE;
}

// ---------------------------------------------------------------- entry points

static PF_Err main_entry(AeEffect *ae, PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                         PF_ParamDef *params[], void *extra) {
    const MsEffect *e = ae->effect;
    switch (cmd) {
    case PF_Cmd_ABOUT:
        snprintf(out_data->return_msg, sizeof out_data->return_msg, "%s\r%s", e->label,
                 e->description);
        return PF_Err_NONE;
    case PF_Cmd_GLOBAL_SETUP:
        out_data->my_version = VERSION;
        // (MsEffect.varying: whatever the settings, After Effects asking once)
        out_data->out_flags =
            OUT_FLAGS | (e->frame_varying || e->varying ? PF_OutFlag_NON_PARAM_VARY : 0);
        out_data->out_flags2 = OUT_FLAGS_2;
        return PF_Err_NONE;
    case PF_Cmd_GLOBAL_SETDOWN:
        ms_effects_release();
        ms_engine_shutdown();
        return PF_Err_NONE;
    case PF_Cmd_PARAMS_SETUP:
        return params_setup(ae, in_data, out_data);
    case PF_Cmd_SMART_PRE_RENDER:
        return pre_render(e, in_data, (PF_PreRenderExtra *)extra);
    case PF_Cmd_SMART_RENDER:
        return smart_render(e, in_data, out_data, (PF_SmartRenderExtra *)extra);
    case PF_Cmd_ARBITRARY_CALLBACK:
        return text_callback(e, in_data, (PF_ArbParamsExtra *)extra);
    case PF_Cmd_USER_CHANGED_PARAM:
        return user_changed_param(e, in_data, out_data, params,
                                  (const PF_UserChangedParamExtra *)extra);
    default:
        return PF_Err_NONE;
    }
}

// One entry point per effect, named in pipl.r.
#define ENTRY(name, slot)                                                                          \
    EXPORT PF_Err EffectMain_##name(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,          \
                                    PF_ParamDef *params[], PF_LayerDef *output, void *extra) {     \
        (void)output;                                                                              \
        return main_entry(&effects[slot], cmd, in_data, out_data, params, extra);                  \
    }
#define MS_EFFECT(Name, name, pipl_id, varies) ENTRY(Name, SLOT_##Name)
#include "effects/list.def"
#undef MS_EFFECT

// One effect for PluginDataEntryFunction2: what the SDK's PF_REGISTER_EFFECT_EXT2 does
// (Util/entry.h, C++ only), as an effect plug-in ('eFKT') with the reserved info it gives.
static PF_Err register_effect(PF_PluginDataPtr in_ptr, PF_PluginDataCB2 callback, const char *name,
                              const char *match_name, const char *entry) {
    const A_long effect_kind = ('e' << 24) | ('F' << 16) | ('K' << 8) | 'T';
    const A_long reserved_info = 8; // AE_RESERVED_INFO
    const A_Err err =
        callback(in_ptr, (const A_u_char *)name, (const A_u_char *)match_name,
                 (const A_u_char *)MS_GROUP, (const A_u_char *)entry, effect_kind,
                 PF_AE_PLUG_IN_VERSION, PF_AE_PLUG_IN_SUBVERS, reserved_info, (const A_u_char *)"");
    return err == A_Err_NONE ? PF_Err_NONE : (PF_Err)err;
}

// Registers the effects with After Effects 2022 and later (the PiPL resources say the
// same for older versions).
EXPORT PF_Err PluginDataEntryFunction2(PF_PluginDataPtr in_ptr, PF_PluginDataCB2 callback,
                                       struct SPBasicSuite *basic, const char *host_name,
                                       const char *host_version) {
    (void)basic;
    (void)host_name;
    (void)host_version;
    // name, match name (saved in projects: keep them), entry point; as in pipl.r
    static const char *const list[][3] = {
#define MS_EFFECT(Name, name, pipl_id, varies)                                                     \
    {"MS_" #Name, "MillSaph " #Name, "EffectMain_" #Name},
#include "effects/list.def"
#undef MS_EFFECT
    };
    PF_Err err = PF_Err_NONE;
    for (size_t i = 0; i < ARRAY_SIZE(list) && !err; ++i)
        err = register_effect(in_ptr, callback, list[i][0], list[i][1], list[i][2]);
    return err;
}
