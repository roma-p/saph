// The viewer overlays of the effects that have one (MsEffect.overlay, see effect.h): ellipses,
// arrows, rays, crosshairs and rectangles drawn from the effects' parameters. The shape under
// the pen is lit; dragging it edits the parameters it is drawn from, so that the shape
// follows the pen. Drawn with the host's Draw Suite when it has one, else OpenGL.

#include <float.h>
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
// Sizes on the screen, in pixels.
#define PICK 8.0        // how near the pen picks a shape
#define ARROW 12.0      // half an arrow's length
#define HEAD_LENGTH 6.0 // an arrow head's length and half width
#define HEAD_WIDTH 2.5
#define CROSSHAIR 12.0 // a crosshair's circle radius (MsOverlay.factor: another)
#define CROSS_IN 0.4   // its cross from 0.4 x the radius
#define CROSS_OUT 2.1  // to 2.1 x the radius
#define SUN_IN 5.0     // a sun's rays: from 5 pixels from the centre
#define SUN_OUT 22.0   // to 22 (the diagonal ones from 11 to 25)
#define SUN_DIAGONAL_IN 11.0
#define SUN_DIAGONAL_OUT 25.0
#define BRACKET 7.0 // a rectangle's corner: two arms this long, then a gap
#define SIDE_GAP 20.0
#define DOT 1.5

static const float LIT[3] = {1.0f, 1.0f, 0.0f}; // the shape under the pen, or dragged

// What a call works on: the effect's instance at a time, and the viewer's scale.
typedef struct {
    const MsEffect *e;
    OfxImageEffectHandle effect;
    OfxParamSetHandle params;
    OfxTime time;
    double pixel[2];  // canonical units per pixel of the screen
    double extent[2]; // the project's size: WIDTH and HEIGHT parameters are fractions of it
    double middle[2]; // the middle of the Source's frame
} Ctx;

static int ctx_init(Ctx *c, const MsEffect *e, OfxPropertySetHandle in_args) {
    OfxPropertySetHandle props;
    memset(c, 0, sizeof *c);
    c->e = e;
    c->pixel[0] = c->pixel[1] = 1.0;
    ms_prop->propGetPointer(in_args, kOfxPropEffectInstance, 0, (void **)&c->effect);
    ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &c->time);
    ms_prop->propGetDoubleN(in_args, kOfxInteractPropPixelScale, 2, c->pixel);
    if (!c->effect || ms_effect->getParamSet(c->effect, &c->params) != kOfxStatOK)
        return 0;
    double offset[2] = {0.0, 0.0};
    if (ms_effect->getPropertySet(c->effect, &props) == kOfxStatOK) {
        ms_prop->propGetDoubleN(props, kOfxImageEffectPropProjectExtent, 2, c->extent);
        ms_prop->propGetDoubleN(props, kOfxImageEffectPropProjectOffset, 2, offset);
    }
    c->middle[0] = offset[0] + 0.5 * c->extent[0];
    c->middle[1] = offset[1] + 0.5 * c->extent[1];
    // The Source's frame where it has one (its region of definition)
    OfxImageClipHandle clip;
    OfxRectD rod;
    if (ms_effect->clipGetHandle(c->effect, "Source", &clip, NULL) == kOfxStatOK &&
        ms_effect->clipGetRegionOfDefinition(clip, c->time, &rod) == kOfxStatOK &&
        rod.x2 > rod.x1 && rod.y2 > rod.y1 && rod.x2 - rod.x1 < 1e7 && rod.y2 - rod.y1 < 1e7) {
        c->middle[0] = 0.5 * (rod.x1 + rod.x2);
        c->middle[1] = 0.5 * (rod.y1 + rod.y2);
    }
    if (c->pixel[0] <= 0.0)
        c->pixel[0] = 1.0;
    if (c->pixel[1] <= 0.0)
        c->pixel[1] = 1.0;
    return 1;
}

// ------------------------------------------------------------------ parameters

static const MsParam *param_info(const MsEffect *e, const char *name) {
    for (int i = 0; i < e->param_count; ++i)
        if (e->params[i].name && strcmp(e->params[i].name, name) == 0)
            return &e->params[i];
    return NULL;
}

static OfxParamHandle param_handle(const Ctx *c, const char *name) {
    OfxParamHandle param;
    if (!name || ms_param->paramGetHandle(c->params, name, &param, NULL) != kOfxStatOK)
        return NULL;
    return param;
}

// A parameter's value (WIDTH and HEIGHT in pixels), `def` without the parameter.
static double get(const Ctx *c, const char *name, double def) {
    const MsParam *p = name ? param_info(c->e, name) : NULL;
    OfxParamHandle param = p ? param_handle(c, name) : NULL;
    if (!param)
        return def;
    if (p->type == MS_PARAM_BOOL || p->type == MS_PARAM_CHOICE) {
        int v = (int)def;
        ms_param->paramGetValueAtTime(param, c->time, &v);
        return v;
    }
    double v = def;
    if (ms_param->paramGetValueAtTime(param, c->time, &v) != kOfxStatOK)
        return def;
    if (p->type == MS_PARAM_WIDTH)
        v *= c->extent[0];
    else if (p->type == MS_PARAM_HEIGHT)
        v *= c->extent[1];
    return v;
}

// Sets a value at the time (a key where the parameter has some), within its limits.
static void set(const Ctx *c, const char *name, double v) {
    const MsParam *p = name ? param_info(c->e, name) : NULL;
    OfxParamHandle param = p ? param_handle(c, name) : NULL;
    if (!param || !isfinite(v))
        return;
    if (p->min != 0.0 || p->max != 0.0) {
        if (p->min > -MS_UNBOUNDED && v < p->min)
            v = p->min;
        if (p->max < MS_UNBOUNDED && v > p->max)
            v = p->max;
    }
    if (p->type == MS_PARAM_WIDTH && c->extent[0] > 0.0)
        v /= c->extent[0];
    else if (p->type == MS_PARAM_HEIGHT && c->extent[1] > 0.0)
        v /= c->extent[1];
    unsigned int keys = 0;
    ms_param->paramGetNumKeys(param, &keys);
    if (keys > 0)
        ms_param->paramSetValueAtTime(param, c->time, v);
    else
        ms_param->paramSetValue(param, v);
}

static int get_point(const Ctx *c, const char *name, double xy[2]) {
    OfxParamHandle param = param_handle(c, name);
    return param && ms_param->paramGetValueAtTime(param, c->time, &xy[0], &xy[1]) == kOfxStatOK;
}

static void set_point(const Ctx *c, const char *name, const double xy[2]) {
    OfxParamHandle param = param_handle(c, name);
    unsigned int keys = 0;
    if (!param)
        return;
    ms_param->paramGetNumKeys(param, &keys);
    if (keys > 0)
        ms_param->paramSetValueAtTime(param, c->time, xy[0], xy[1]);
    else
        ms_param->paramSetValue(param, xy[0], xy[1]);
}

// ------------------------------------------------------------------ shapes

// A shape where the parameters put it.
typedef struct {
    double c[2];       // its centre
    double r, rx, ry;  // its length, across and up
    double a[2], b[2]; // the points `point` and `point2`
} Shape;

static int shape_at(const Ctx *c, const MsOverlay *o, Shape *s) {
    memset(s, 0, sizeof *s);
    if (o->show && get(c, o->show, 1.0) == 0.0)
        return 0;
    if (o->point) {
        if (!get_point(c, o->point, s->a))
            return 0;
        s->b[0] = s->a[0];
        s->b[1] = s->a[1];
        if (o->point2 && !get_point(c, o->point2, s->b))
            return 0;
        s->c[0] = 0.5 * (s->a[0] + s->b[0]);
        s->c[1] = 0.5 * (s->a[1] + s->b[1]);
    } else {
        s->c[0] = c->middle[0];
        s->c[1] = c->middle[1];
    }
    s->r = (o->factor != 0.0 ? o->factor : 1.0) * get(c, o->size, 1.0) * get(c, o->scale, 1.0);
    s->rx = s->r * get(c, o->rel_x, 1.0);
    s->ry = s->r * get(c, o->rel_y, 1.0);
    return 1;
}

// The ends of a RECT's corners and sides: corner k (0 bottom left, then anticlockwise) and
// side k (0 bottom, 1 right, 2 top, 3 left: from corner k to corner k + 1).
static void rect_corners(const Shape *s, double corner[4][2]) {
    const double x0 = fmin(s->a[0], s->b[0]), x1 = fmax(s->a[0], s->b[0]);
    const double y0 = fmin(s->a[1], s->b[1]), y1 = fmax(s->a[1], s->b[1]);
    const double xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    memcpy(corner, xy, sizeof xy);
}

// ------------------------------------------------------------------ drawing

typedef struct {
    OfxDrawContextHandle draw; // the Draw Suite's context, NULL: OpenGL
    const Ctx *c;
} Pen;

static void set_colour(const Pen *p, const float rgb[3]) {
    if (p->draw) {
        const OfxRGBAColourF colour = {rgb[0], rgb[1], rgb[2], 1.0f};
        ms_draw->setColour(p->draw, &colour);
    } else {
        glColor4f(rgb[0], rgb[1], rgb[2], 1.0f);
    }
}

// mode: GL_LINES, GL_LINE_LOOP or GL_POLYGON
static void draw_points(const Pen *p, GLenum mode, const OfxPointD *pts, int count) {
    if (p->draw) {
        ms_draw->draw(p->draw,
                      mode == GL_LINES       ? kOfxDrawPrimitiveLines
                      : mode == GL_LINE_LOOP ? kOfxDrawPrimitiveLineLoop
                                             : kOfxDrawPrimitivePolygon,
                      pts, count);
        return;
    }
    glBegin(mode);
    for (int i = 0; i < count; ++i)
        glVertex2d(pts[i].x, pts[i].y);
    glEnd();
}

// A line from (x0, y0) to (x1, y1), canonical coordinates.
static void line(const Pen *p, double x0, double y0, double x1, double y1) {
    const OfxPointD pts[2] = {{x0, y0}, {x1, y1}};
    draw_points(p, GL_LINES, pts, 2);
}

// An ellipse (rx, ry canonical), or a circle of `pixels` on the screen when rx is 0.
static void ellipse(const Pen *p, const double c[2], double rx, double ry, double pixels,
                    GLenum mode) {
    enum { N = 128 };
    OfxPointD pts[N];
    if (pixels > 0.0) {
        rx = pixels * p->c->pixel[0];
        ry = pixels * p->c->pixel[1];
    }
    for (int i = 0; i < N; ++i) {
        const double a = 2.0 * PI * i / N;
        pts[i].x = c[0] + rx * cos(a);
        pts[i].y = c[1] + ry * sin(a);
    }
    draw_points(p, mode, pts, N);
}

// A double arrow across (ux, uy) (a unit vector) through (x, y), sized on the screen.
static void arrow(const Pen *p, double x, double y, double ux, double uy) {
    const double sx = p->c->pixel[0], sy = p->c->pixel[1];
    line(p, x - ARROW * ux * sx, y - ARROW * uy * sy, x + ARROW * ux * sx, y + ARROW * uy * sy);
    for (int end = -1; end <= 1; end += 2) {
        const double tx = x + end * ARROW * ux * sx, ty = y + end * ARROW * uy * sy;
        for (int side = -1; side <= 1; side += 2) {
            const double bx = tx - end * HEAD_LENGTH * ux * sx + side * HEAD_WIDTH * -uy * sx;
            const double by = ty - end * HEAD_LENGTH * uy * sy + side * HEAD_WIDTH * ux * sy;
            line(p, tx, ty, bx, by);
        }
    }
}

// A segment of the screen-sized rays round c: from `in` to `out` pixels along (ux, uy).
static void ray_pixels(const Pen *p, const double c[2], double ux, double uy, double in,
                       double out) {
    const double sx = p->c->pixel[0], sy = p->c->pixel[1];
    line(p, c[0] + in * ux * sx, c[1] + in * uy * sy, c[0] + out * ux * sx, c[1] + out * uy * sy);
}

static void draw_shape(const Pen *p, const MsOverlay *o, const Shape *s, int lit) {
    const Ctx *c = p->c;
    set_colour(p, lit ? LIT : o->colour);
    switch (o->kind) {
    case MS_OVERLAY_ELLIPSE:
        ellipse(p, s->c, s->rx, s->ry, 0.0, GL_LINE_LOOP);
        break;
    case MS_OVERLAY_ARROW_X:
        arrow(p, s->c[0] + s->rx, s->c[1], 1.0, 0.0);
        break;
    case MS_OVERLAY_ARROW_Y:
        arrow(p, s->c[0], s->c[1] + s->ry, 0.0, 1.0);
        break;
    case MS_OVERLAY_RAY:
        line(p, s->c[0] - s->r * o->dir[0], s->c[1] - s->r * o->dir[1], s->c[0] + s->r * o->dir[0],
             s->c[1] + s->r * o->dir[1]);
        break;
    case MS_OVERLAY_DOT:
        ellipse(p, s->c, 0.0, 0.0, DOT, GL_POLYGON);
        break;
    case MS_OVERLAY_CROSSHAIR: {
        const double radius = o->factor != 0.0 ? o->factor : CROSSHAIR;
        ellipse(p, s->c, 0.0, 0.0, radius, GL_LINE_LOOP);
        for (int k = 0; k < 4; ++k)
            ray_pixels(p, s->c, cos(k * PI / 2), sin(k * PI / 2), CROSS_IN * radius,
                       CROSS_OUT * radius);
        break;
    }
    case MS_OVERLAY_SUN:
        for (int k = 0; k < 8; ++k) {
            const double a = k * PI / 4;
            if (k % 2 == 0)
                ray_pixels(p, s->c, cos(a), sin(a), SUN_IN, SUN_OUT);
            else
                ray_pixels(p, s->c, cos(a), sin(a), SUN_DIAGONAL_IN, SUN_DIAGONAL_OUT);
        }
        break;
    case MS_OVERLAY_RECT: {
        double corner[4][2];
        rect_corners(s, corner);
        for (int k = 0; k < 4; ++k) {
            const double *from = corner[k], *to = corner[(k + 1) % 4];
            // in screen pixels along the side
            const double dx = (to[0] - from[0]) / c->pixel[0], dy = (to[1] - from[1]) / c->pixel[1];
            const double length = sqrt(dx * dx + dy * dy);
            if (length <= 2.0 * SIDE_GAP) {
                line(p, from[0], from[1], to[0], to[1]);
                continue;
            }
            const double ux = dx / length * c->pixel[0], uy = dy / length * c->pixel[1];
            line(p, from[0], from[1], from[0] + BRACKET * ux, from[1] + BRACKET * uy);
            line(p, from[0] + SIDE_GAP * ux, from[1] + SIDE_GAP * uy, to[0] - SIDE_GAP * ux,
                 to[1] - SIDE_GAP * uy);
            line(p, to[0] - BRACKET * ux, to[1] - BRACKET * uy, to[0], to[1]);
        }
        break;
    }
    }
}

// ------------------------------------------------------------------ picking

// Distance from p to the segment a-b, all in screen pixels.
static double segment_distance(const double p[2], const double a[2], const double b[2]) {
    const double abx = b[0] - a[0], aby = b[1] - a[1];
    const double l2 = abx * abx + aby * aby;
    double t = l2 > 0.0 ? ((p[0] - a[0]) * abx + (p[1] - a[1]) * aby) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    const double dx = p[0] - a[0] - t * abx, dy = p[1] - a[1] - t * aby;
    return sqrt(dx * dx + dy * dy);
}

// How far the pen at `at` (canonical) is from a shape, in screen pixels (DBL_MAX: it cannot
// be picked); *part: which part of a RECT (corners 0-3, sides 4-7).
static double distance(const Ctx *c, const MsOverlay *o, const Shape *s, const double at[2],
                       int *part) {
    const double sx = c->pixel[0], sy = c->pixel[1];
    const double p[2] = {(at[0] - s->c[0]) / sx, (at[1] - s->c[1]) / sy}; // from the centre
    const double from_centre = sqrt(p[0] * p[0] + p[1] * p[1]);
    *part = 0;
    switch (o->kind) {
    case MS_OVERLAY_ELLIPSE: {
        const double rx = fabs(s->rx) / sx, ry = fabs(s->ry) / sy;
        if (rx < 1e-9 || ry < 1e-9)
            return from_centre;
        const double q = sqrt(p[0] * p[0] / (rx * rx) + p[1] * p[1] / (ry * ry));
        return q < 1e-9 ? fmin(rx, ry) : from_centre * fabs(1.0 - 1.0 / q);
    }
    case MS_OVERLAY_ARROW_X:
    case MS_OVERLAY_ARROW_Y: {
        const int x = o->kind == MS_OVERLAY_ARROW_X;
        const double mid[2] = {x ? s->rx / sx : 0.0, x ? 0.0 : s->ry / sy};
        const double a[2] = {mid[0] - (x ? ARROW : 0.0), mid[1] - (x ? 0.0 : ARROW)};
        const double b[2] = {mid[0] + (x ? ARROW : 0.0), mid[1] + (x ? 0.0 : ARROW)};
        return segment_distance(p, a, b);
    }
    case MS_OVERLAY_RAY: {
        const double a[2] = {-s->r * o->dir[0] / sx, -s->r * o->dir[1] / sy};
        const double b[2] = {-a[0], -a[1]};
        return segment_distance(p, a, b);
    }
    case MS_OVERLAY_DOT:
        return DBL_MAX;
    case MS_OVERLAY_CROSSHAIR: {
        const double radius = (o->factor != 0.0 ? o->factor : CROSSHAIR) * CROSS_OUT;
        return from_centre <= radius ? 0.0 : from_centre - radius;
    }
    case MS_OVERLAY_SUN:
        return from_centre <= SUN_DIAGONAL_OUT ? 0.0 : from_centre - SUN_DIAGONAL_OUT;
    case MS_OVERLAY_RECT: {
        double corner[4][2], q[4][2];
        rect_corners(s, corner);
        for (int k = 0; k < 4; ++k) {
            q[k][0] = (corner[k][0] - s->c[0]) / sx;
            q[k][1] = (corner[k][1] - s->c[1]) / sy;
        }
        double best = DBL_MAX;
        for (int k = 0; k < 4; ++k) { // the corners first
            const double d = segment_distance(p, q[k], q[k]);
            if (d < best && d <= PICK)
                best = d, *part = k;
        }
        if (best <= PICK)
            return 0.0;
        for (int k = 0; k < 4; ++k) {
            const double d = segment_distance(p, q[k], q[(k + 1) % 4]);
            if (d < best)
                best = d, *part = 4 + k;
        }
        return best;
    }
    }
    return DBL_MAX;
}

// The shape under the pen (-1: none) and its part: the nearest, the handles (arrows, rays,
// crosshairs...) before the ellipses they cross.
static int pick(const Ctx *c, const double at[2], int *part) {
    for (int ellipses = 0; ellipses <= 1; ++ellipses) {
        double best = PICK;
        int found = -1;
        for (int i = 0; i < c->e->overlay_count; ++i) {
            const MsOverlay *o = &c->e->overlay[i];
            Shape s;
            int k;
            if ((o->kind == MS_OVERLAY_ELLIPSE) != ellipses || !shape_at(c, o, &s))
                continue;
            const double d = distance(c, o, &s, at, &k);
            if (d <= best) {
                best = d;
                found = i;
                *part = k;
            }
        }
        if (found >= 0)
            return found;
    }
    return -1;
}

// ------------------------------------------------------------------ state

// The shape lit or dragged, in one effect instance (one pen at a time in a host).
static struct {
    OfxImageEffectHandle effect;
    int hover;    // the shape under the pen (-1: none)
    int dragging; // 1 while a shape is dragged (the `hover` one)
    int part;
    double pen[2]; // where the drag started
    Shape start;   // the shape then
    double value;  // the value it edits then
} state = {.effect = NULL, .hover = -1};

static int lit_shape(OfxImageEffectHandle effect) {
    return state.effect == effect ? state.hover : -1;
}

// ------------------------------------------------------------------ actions

static OfxStatus draw_overlay(const MsEffect *e, OfxPropertySetHandle in_args) {
    Ctx c;
    if (!ctx_init(&c, e, in_args))
        return kOfxStatReplyDefault;
    Pen p = {NULL, &c};
    if (ms_draw)
        ms_prop->propGetPointer(in_args, kOfxInteractPropDrawContext, 0, (void **)&p.draw);
    if (p.draw) {
        ms_draw->setLineWidth(p.draw, 1.0f);
    } else {
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
        glLineWidth(1.0f);
    }
    const int lit = lit_shape(c.effect);
    for (int i = 0; i < e->overlay_count; ++i) {
        Shape s;
        if (shape_at(&c, &e->overlay[i], &s))
            draw_shape(&p, &e->overlay[i], &s, i == lit);
    }
    if (!p.draw)
        glPopAttrib();
    return kOfxStatOK;
}

static void redraw(const void *handle) {
    if (ms_interact && handle)
        ms_interact->interactRedraw((OfxInteractHandle)handle);
}

// The parameter a drag of the shape edits (ELLIPSE and RAY).
static const char *scaled(const MsOverlay *o) { return o->scale ? o->scale : o->size; }

// The shape's parameters for the pen at `at`, from the drag's start.
static void drag_to(const Ctx *c, const MsOverlay *o, const double at[2]) {
    const Shape *s = &state.start;
    const double dx = at[0] - state.pen[0], dy = at[1] - state.pen[1];
    switch (o->kind) {
    case MS_OVERLAY_ELLIPSE: { // the ellipse through the pen
        if (fabs(s->rx) < 1e-9 || fabs(s->ry) < 1e-9)
            return;
        const double x0 = (state.pen[0] - s->c[0]) / s->rx, y0 = (state.pen[1] - s->c[1]) / s->ry;
        const double x1 = (at[0] - s->c[0]) / s->rx, y1 = (at[1] - s->c[1]) / s->ry;
        const double q0 = sqrt(x0 * x0 + y0 * y0), q1 = sqrt(x1 * x1 + y1 * y1);
        if (q0 > 1e-6)
            set(c, scaled(o), state.value * q1 / q0);
        break;
    }
    case MS_OVERLAY_ARROW_X:
        if (fabs(s->r) > 1e-9)
            set(c, o->rel_x, state.value + dx / s->r);
        break;
    case MS_OVERLAY_ARROW_Y:
        if (fabs(s->r) > 1e-9)
            set(c, o->rel_y, state.value + dy / s->r);
        break;
    case MS_OVERLAY_RAY: { // the ray's end on the pen, along the ray
        const double d2 = o->dir[0] * o->dir[0] + o->dir[1] * o->dir[1];
        const double t0 =
            ((state.pen[0] - s->c[0]) * o->dir[0] + (state.pen[1] - s->c[1]) * o->dir[1]) / d2;
        const double t1 = ((at[0] - s->c[0]) * o->dir[0] + (at[1] - s->c[1]) * o->dir[1]) / d2;
        if (fabs(t0) > 1e-6)
            set(c, scaled(o), state.value * t1 / t0);
        break;
    }
    case MS_OVERLAY_DOT:
        break;
    case MS_OVERLAY_CROSSHAIR:
    case MS_OVERLAY_SUN: {
        const double a[2] = {s->a[0] + dx, s->a[1] + dy}, b[2] = {s->b[0] + dx, s->b[1] + dy};
        set_point(c, o->point, a);
        if (o->point2)
            set_point(c, o->point2, b);
        break;
    }
    case MS_OVERLAY_RECT: {
        // which of left, right, bottom, top the part moves
        static const int moves[8][4] = {{1, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 1}, {1, 0, 0, 1},
                                        {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 0}};
        const int *m = moves[state.part];
        double a[2] = {s->a[0], s->a[1]}, b[2] = {s->b[0], s->b[1]};
        const int a_left = s->a[0] <= s->b[0], a_bottom = s->a[1] <= s->b[1];
        if (m[0])
            *(a_left ? &a[0] : &b[0]) += dx;
        if (m[1])
            *(a_left ? &b[0] : &a[0]) += dx;
        if (m[2])
            *(a_bottom ? &a[1] : &b[1]) += dy;
        if (m[3])
            *(a_bottom ? &b[1] : &a[1]) += dy;
        set_point(c, o->point, a);
        set_point(c, o->point2, b);
        break;
    }
    }
}

// The value a drag of the shape edits, at its start.
static double edited_value(const Ctx *c, const MsOverlay *o) {
    switch (o->kind) {
    case MS_OVERLAY_ELLIPSE:
    case MS_OVERLAY_RAY:
        return get(c, scaled(o), 1.0);
    case MS_OVERLAY_ARROW_X:
        return get(c, o->rel_x, 1.0);
    case MS_OVERLAY_ARROW_Y:
        return get(c, o->rel_y, 1.0);
    default:
        return 0.0;
    }
}

static OfxStatus pen(const MsEffect *e, const void *handle, const char *action,
                     OfxPropertySetHandle in_args) {
    Ctx c;
    double at[2];
    if (!ctx_init(&c, e, in_args) ||
        ms_prop->propGetDoubleN(in_args, kOfxInteractPropPenPosition, 2, at) != kOfxStatOK)
        return kOfxStatReplyDefault;
    const int down = strcmp(action, kOfxInteractActionPenDown) == 0;
    const int up = strcmp(action, kOfxInteractActionPenUp) == 0;
    const int dragging = state.dragging && state.effect == c.effect;

    if (!dragging) {
        int part = 0;
        const int found = pick(&c, at, &part);
        if (found != lit_shape(c.effect) || state.effect != c.effect) {
            state.effect = c.effect;
            state.hover = found;
            redraw(handle);
        }
        if (!down || found < 0)
            return kOfxStatReplyDefault;
        // a drag starts
        const MsOverlay *o = &e->overlay[found];
        state.dragging = 1;
        state.part = part;
        state.pen[0] = at[0];
        state.pen[1] = at[1];
        shape_at(&c, o, &state.start);
        state.value = edited_value(&c, o);
        ms_param->paramEditBegin(c.params, "Drag");
        return kOfxStatOK;
    }
    drag_to(&c, &e->overlay[state.hover], at);
    if (up) {
        ms_param->paramEditEnd(c.params);
        state.dragging = 0;
    }
    redraw(handle);
    return kOfxStatOK;
}

OfxStatus ms_overlay_main(const MsEffect *e, const char *action, const void *handle,
                          OfxPropertySetHandle in_args, OfxPropertySetHandle out_args) {
    (void)out_args;
    if (!e->overlay || e->overlay_count <= 0)
        return kOfxStatReplyDefault;
    if (strcmp(action, kOfxActionDescribe) == 0 || strcmp(action, kOfxActionCreateInstance) == 0)
        return kOfxStatOK;
    if (strcmp(action, kOfxActionDestroyInstance) == 0) {
        state.effect = NULL;
        state.hover = -1;
        state.dragging = 0;
        return kOfxStatOK;
    }
    if (strcmp(action, kOfxInteractActionDraw) == 0)
        return draw_overlay(e, in_args);
    if (strcmp(action, kOfxInteractActionPenDown) == 0 ||
        strcmp(action, kOfxInteractActionPenMotion) == 0 ||
        strcmp(action, kOfxInteractActionPenUp) == 0)
        return pen(e, handle, action, in_args);
    if (strcmp(action, kOfxInteractActionLoseFocus) == 0 && !state.dragging && state.hover >= 0) {
        state.hover = -1;
        redraw(handle);
    }
    return kOfxStatReplyDefault;
}
