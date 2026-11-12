// The viewer overlay of the effects that ask for one (MsEffect.overlay): a line between two
// POINT parameters (MS_LensFlare: the axis from the hotspot to the pivot), and handles to
// drag the points the host has no handle of its own for. Drawn with the host's Draw Suite
// when it has one, else OpenGL.

#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "ofxDrawSuite.h"
#include "ofxInteract.h"

#include "common/defines.h"
#include "ofx/host.h"

#define PI 3.14159265358979323846
#define HANDLE_PIXELS 7.0 // a handle's radius on the screen, and how near a click picks it

// The two points of the overlay at `time` (canonical coordinates), and whether the host
// draws each one's handle itself.
typedef struct {
    double xy[2][2];
    int host_handle[2];
} Points;

static OfxParamHandle point_param(OfxImageEffectHandle effect, const char *name) {
    OfxParamSetHandle params;
    OfxParamHandle param;
    if (ms_effect->getParamSet(effect, &params) != kOfxStatOK ||
        ms_param->paramGetHandle(params, name, &param, NULL) != kOfxStatOK)
        return NULL;
    return param;
}

static int get_points(const MsEffect *e, OfxImageEffectHandle effect, OfxTime time, Points *p) {
    for (int k = 0; k < 2; ++k) {
        OfxParamHandle param = point_param(effect, e->overlay[k]);
        OfxPropertySetHandle props;
        int host = 0;
        if (!param || ms_param->paramGetValueAtTime(param, time, &p->xy[k][0], &p->xy[k][1]) != kOfxStatOK)
            return 0;
        if (ms_param->paramGetPropertySet(param, &props) == kOfxStatOK)
            ms_prop->propGetInt(props, kOfxParamPropHasHostOverlayHandle, 0, &host);
        p->host_handle[k] = host;
    }
    return 1;
}

// ------------------------------------------------------------------ drawing

static void draw_points(OfxDrawContextHandle draw, const OfxDrawSuiteV1 *suite, GLenum mode,
                        const OfxPointD *pts, int count, const float rgba[4]) {
    if (draw && suite) {
        const OfxRGBAColourF colour = {rgba[0], rgba[1], rgba[2], rgba[3]};
        suite->setColour(draw, &colour);
        suite->draw(draw, mode == GL_LINES ? kOfxDrawPrimitiveLines : kOfxDrawPrimitiveLineLoop,
                    pts, count);
        return;
    }
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glBegin(mode);
    for (int i = 0; i < count; ++i)
        glVertex2d(pts[i].x, pts[i].y);
    glEnd();
}

static OfxStatus draw_overlay(const MsEffect *e, OfxPropertySetHandle in_args) {
    OfxImageEffectHandle effect = NULL;
    OfxTime time = 0;
    double scale[2] = {1, 1};
    ms_prop->propGetPointer(in_args, kOfxPropEffectInstance, 0, (void **)&effect);
    ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time);
    ms_prop->propGetDoubleN(in_args, kOfxInteractPropPixelScale, 2, scale);
    Points p;
    if (!effect || !get_points(e, effect, time, &p))
        return kOfxStatReplyDefault;
    OfxDrawContextHandle draw = NULL;
    const OfxDrawSuiteV1 *suite = ms_draw;
    if (suite)
        ms_prop->propGetPointer(in_args, kOfxInteractPropDrawContext, 0, (void **)&draw);
    if (!draw)
        glPushAttrib(GL_ALL_ATTRIB_BITS);

    // the line, dark under light so it shows on any image
    const OfxPointD line[2] = {{p.xy[0][0], p.xy[0][1]}, {p.xy[1][0], p.xy[1][1]}};
    const float shadow[4] = {0, 0, 0, 0.6f}, light[4] = {1, 0.85f, 0.4f, 0.9f};
    if (draw && suite)
        suite->setLineWidth(draw, 3);
    else
        glLineWidth(3);
    draw_points(draw, suite, GL_LINES, line, 2, shadow);
    if (draw && suite)
        suite->setLineWidth(draw, 1);
    else
        glLineWidth(1);
    draw_points(draw, suite, GL_LINES, line, 2, light);
    // the handles the host does not draw: circles
    for (int k = 0; k < 2; ++k) {
        if (p.host_handle[k])
            continue;
        OfxPointD circle[24];
        for (int i = 0; i < 24; ++i) {
            const double a = 2 * PI * i / 24;
            circle[i].x = p.xy[k][0] + HANDLE_PIXELS * scale[0] * cos(a);
            circle[i].y = p.xy[k][1] + HANDLE_PIXELS * scale[1] * sin(a);
        }
        draw_points(draw, suite, GL_LINE_LOOP, circle, 24, light);
    }
    if (!draw)
        glPopAttrib();
    return kOfxStatOK;
}

// ------------------------------------------------------------------ dragging

static OfxParamSetHandle effect_param_set(OfxImageEffectHandle effect) {
    OfxParamSetHandle params = NULL;
    ms_effect->getParamSet(effect, &params);
    return params;
}

static OfxStatus pen(const MsEffect *e, OfxInteractHandle interact, const char *action,
                     OfxPropertySetHandle in_args) {
    static int dragging = -1; // the point dragged (-1: none; one pen at a time in a host)
    OfxImageEffectHandle effect = NULL;
    OfxTime time = 0;
    double scale[2] = {1, 1}, at[2];
    (void)interact;
    ms_prop->propGetPointer(in_args, kOfxPropEffectInstance, 0, (void **)&effect);
    ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time);
    ms_prop->propGetDoubleN(in_args, kOfxInteractPropPixelScale, 2, scale);
    if (ms_prop->propGetDoubleN(in_args, kOfxInteractPropPenPosition, 2, at) != kOfxStatOK)
        return kOfxStatReplyDefault;
    Points p;
    if (!effect || !get_points(e, effect, time, &p))
        return kOfxStatReplyDefault;
    if (strcmp(action, kOfxInteractActionPenDown) == 0) {
        dragging = -1;
        for (int k = 0; k < 2; ++k) {
            const double dx = (at[0] - p.xy[k][0]) / scale[0], dy = (at[1] - p.xy[k][1]) / scale[1];
            if (!p.host_handle[k] && dx * dx + dy * dy <= 2.0 * HANDLE_PIXELS * HANDLE_PIXELS)
                dragging = k;
        }
        if (dragging < 0)
            return kOfxStatReplyDefault;
        ms_param->paramEditBegin(effect_param_set(effect), e->overlay[dragging]);
        return kOfxStatOK;
    }
    if (dragging < 0)
        return kOfxStatReplyDefault;
    OfxParamHandle param = point_param(effect, e->overlay[dragging]);
    if (param)
        ms_param->paramSetValueAtTime(param, time, at[0], at[1]);
    if (strcmp(action, kOfxInteractActionPenUp) == 0) {
        ms_param->paramEditEnd(effect_param_set(effect));
        dragging = -1;
    }
    return kOfxStatOK;
}

OfxStatus ms_overlay_main(const MsEffect *e, const char *action, const void *handle,
                          OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {
    (void)out_args;
    if (!e->overlay[0] || !e->overlay[1])
        return kOfxStatReplyDefault;
    if (strcmp(action, kOfxActionDescribe) == 0 || strcmp(action, kOfxActionCreateInstance) == 0 ||
        strcmp(action, kOfxActionDestroyInstance) == 0)
        return kOfxStatOK;
    if (strcmp(action, kOfxInteractActionDraw) == 0)
        return draw_overlay(e, in_args);
    if (strcmp(action, kOfxInteractActionPenDown) == 0 ||
        strcmp(action, kOfxInteractActionPenMotion) == 0 ||
        strcmp(action, kOfxInteractActionPenUp) == 0)
        return pen(e, (OfxInteractHandle)handle, action, in_args);
    return kOfxStatReplyDefault;
}
