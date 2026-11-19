// MS_TVDamage in Halide, called by effects/ms_tvdamage.c with its settings in one row
// (tv_damage_layout.h). In order:
//   - PictureSource (ms_tv_damage_source): the reception: ghosts (copies shifted across, spread
//     across by Blur, their weight taken from the picture, or subtracted: negative ghosts),
//     then where each pixel
//     reads: the vertical hold's roll with its black bar (and sync dashes), the horizontal
//     hold's row moves (fractal noise over the rows and time, wrapped, a black border at the
//     wrap), the fast forward's torn bands (rows stretched sideways, black past the right)
//   - Bright (ms_tv_damage_bright): what the orthicon darkens around, max(0, luma - threshold),
//     blurred twice by ms_blur (a narrow and a wide blur)
//   - Screen (ms_tv_damage_screen): the orthicon's dark halo (wide - narrow blur, where
//     positive); static (per pixel, a share of the pixels, a little wider than tall) and
//     interference dots (a share of small cells, moving), added; colour stripes (a plane wave,
//     R G B a third of a cycle apart), multiplied; the colour correction (hue in HSV with the
//     luma kept, saturation, darks + max(lights - darks, 0) x colour); bars (a rolling
//     envelope times a faster carrier) and tape dropouts (white streaks), added; scanlines
//     and the vignette, multiplied
//   - Final (ms_tv_damage_final): the picture's end: turn off (a shrinking lit box with
//     trails, then a line and a spot, fading), fish eye (bilinear, black outside), TV pixels
//     (each pixel its cell of the area means, ms_pixel_sort_down), mixed with the Source by
//     the mask value
// The random parts use MillSaph's own noise (noise.h), at the reference's measured scales.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "hue.h"
#include "noise.h"
#include "shared.h"
#include "tv_damage_layout.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

using ms_noise::hash3;
using ms_noise::symmetric;
using ms_noise::uniform;

const float TAU = 6.28318530718f;

Expr luma(Expr r, Expr g, Expr b) { return (5.0f * r + 8.0f * g + 3.0f * b) / 16.0f; }

// A setting.
struct Settings {
    Func row;
    Expr operator()(int k) const { return row(k, 0); }
    Expr i(int k) const { return cast<int>(row(k, 0)); }
};

// The four channels of an image sampled at (sx, sy) (pixel centres at i + 0.5): bilinear,
// black outside.
Expr bilinear(Func in, Expr sx, Expr sy, Expr w, Expr h) {
    Expr fx = sx - 0.5f, fy = sy - 0.5f;
    Expr x0 = cast<int>(floor(fx)), y0 = cast<int>(floor(fy));
    Expr ax = fx - floor(fx), ay = fy - floor(fy);
    auto at = [&](Expr u, Expr v) {
        return select(u >= 0 && u < w && v >= 0 && v < h,
                      in(clamp(u, 0, w - 1), clamp(v, 0, h - 1), c), 0.0f);
    };
    Expr top = at(x0, y0 + 1) * (1.0f - ax) + at(x0 + 1, y0 + 1) * ax;
    Expr bottom = at(x0, y0) * (1.0f - ax) + at(x0 + 1, y0) * ax;
    return bottom * (1.0f - ay) + top * ay;
}

} // namespace

// The reception: ghosts, holds, fast forward.
class TVDamageSource : public Generator<TVDamageSource> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> settings{"settings"};
    Input<int> ghosts{"ghosts"};   // TV_GHOSTS (an RDom's extent)
    Input<int> octaves{"octaves"}; // TV_HHOLD_OCTAVES
    Output<Buffer<float, 3>> picture{"picture"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Settings s{settings};
        Expr w = source.dim(0).extent(), h = source.dim(1).extent();
        Expr fw = cast<float>(w), fh = cast<float>(h);
        Expr seed = s.i(TV_SEED), t = s(TV_TIME);
        Func in = BoundaryConditions::constant_exterior(source, 0.0f);

        // Ghosts: evenly spread from the start over the span (one: in the middle), each moved at
        // random by Vary Position; raw weights a = weight x a random number in 0..1; negative
        // ones spread evenly by their share (ghost k when floor((k + 1) share) > floor(k
        // share)), at half their weight; the picture and the ghosts divided by 1 + the sum of
        // their weights, if above 0 (the picture's level kept).
        RDom k(0, ghosts, "k");
        auto rnd = [&](Expr salt) { return uniform(hash3(k, salt, seed + 1301)); };
        Expr fk = cast<float>(k), share = s(TV_GHOST_NEGATIVE);
        Expr place = select(ghosts > 1, fk / cast<float>(max(ghosts - 1, 1)), 0.5f);
        Expr offset = s(TV_GHOST_START) + s(TV_GHOST_SPAN) * place +
                      s(TV_GHOST_VARY) * (rnd(1) - 0.5f);
        Expr negative = floor((fk + 1.0f) * share) > floor(fk * share);
        Expr raw = s(TV_GHOST_WEIGHT) * rnd(3);
        Func taken("taken");
        Expr weight = select(negative, -0.5f, 1.0f) * raw;
        taken() = sum(weight);
        Expr scale = 1.0f / (1.0f + max(taken(), 0.0f));
        Expr gx = cast<float>(x) - offset, g0 = floor(gx), ga = gx - g0;
        // (Blur: the shifted picture's 1 and 2 pixels to the sides weighted in)
        auto at = [&](Expr dx) {
            Expr i = cast<int>(g0) + dx;
            return in(i, y, c) * (1.0f - ga) + in(i + 1, y, c) * ga;
        };
        Expr b1 = s(TV_GHOST_BLUR1), b2 = s(TV_GHOST_BLUR2);
        Expr shifted = at(0) * (1.0f - 2.0f * (b1 + b2)) + (at(-1) + at(1)) * b1 +
                       select(b2 > 0.0f, (at(-2) + at(2)) * b2, 0.0f);
        Func ghosted("ghosted");
        ghosted(x, y, c) =
            select(c < 3, (in(x, y, c) + sum(weight * shifted)) * scale, in(x, y, c));

        // Vertical hold: the rows roll over the picture and a black bar.
        Expr bar = s(TV_VHOLD_BAR), on = s(TV_VHOLD_ON) > 0.5f;
        Expr rolled = cast<float>(y) + 0.5f + s(TV_VHOLD_OFFSET);
        Expr span = fh + bar;
        rolled = rolled - span * floor(rolled / span);
        Expr in_bar = on && rolled >= fh;
        Expr row = select(on, min(rolled, fh - 0.5f), cast<float>(y) + 0.5f);

        // Horizontal hold: each row's move.
        Func hold("hold");
        hold(y) = s(TV_HHOLD_AMP) / 0.9f *
                  ms_noise::fbm((cast<float>(y) + 0.5f) / fh * s(TV_HHOLD_FREQ), 7.31f,
                                s(TV_HHOLD_Z), octaves, 0.5f, 10, "octave");

        // Fast forward: rows in the bands stretched sideways at random.
        Expr band = (cast<float>(y) + 0.5f) / fh * s(TV_FF_BANDS) + s(TV_FF_PHASE);
        Expr inside = band - floor(band);
        Expr half = 0.5f * s(TV_FF_HEIGHT);
        Expr profile = select(inside < half, 1.0f - inside / max(half, 1e-6f), 0.0f);
        Expr tear = uniform(hash3(y, cast<int>(floor(band)), seed + cast<int>(t) * 31 + 77));
        Expr stretch = s(TV_FF_AMP) * s(TV_FF_SHIFT) * tear * profile * profile;

        Expr sx = (cast<float>(x) + 0.5f) * (1.0f + stretch) -
                  hold(clamp(cast<int>(floor(row)), 0, h - 1));
        Expr border = s(TV_HHOLD_BORDER);
        Expr wrapped = sx - fw * floor(sx / fw);
        Expr black = in_bar || (sx < 0.0f && sx >= -border) || (sx >= fw && sx < fw + border) ||
                     (stretch > 0.0f && sx >= fw);
        Expr ix = clamp(cast<int>(floor(select(stretch > 0.0f, sx, wrapped))), 0, w - 1);
        Expr iy = clamp(cast<int>(floor(row)), 0, h - 1);
        // The bar's sync data: short white dashes on every other row; the fast forward's
        // bands: white dashes too.
        Expr bar_row = cast<int>(floor(rolled));
        Expr dash = bar_row % 2 == 0 &&
                    uniform(hash3(bar_row, x / 3, seed + 911)) < 0.2f * s(TV_VHOLD_DATA);
        Expr ff_dash = stretch > 0.0f &&
                       uniform(hash3(x / 5, y, seed + cast<int>(t) * 13 + 97)) < 0.35f * profile;
        picture(x, y, c) = select(in_bar, select(c < 3 && dash, 0.8f, select(c == 3, 1.0f, 0.0f)),
                                  black, select(c == 3, ghosted(ix, iy, 3), 0.0f),
                                  ff_dash && c < 3, 0.85f, ghosted(ix, iy, c));

        ms::interleaved(source);
        settings.dim(0).set_bounds(0, TV_FIELDS);
        ms::interleaved(picture);

        Var xo("xo"), xi("xi"), yo("yo"), yi("yi");
        taken.compute_root();
        hold.compute_root();
        if (gpu) {
            taken.gpu_single_thread();
            hold.gpu_tile(y, yo, yi, 64);
        }
        ghosted.compute_root();
        ms::schedule_rgba(ghosted, gpu);
        ms::schedule_rgba(picture, gpu);
    }
};

// What the orthicon darkens around.
class TVDamageBright : public Generator<TVDamageBright> {
  public:
    Input<Buffer<float, 3>> picture{"picture"};
    Input<Buffer<float, 2>> settings{"settings"};
    Output<Buffer<float, 3>> bright{"bright"};

    void generate() {
        Settings s{settings};
        Expr l = luma(picture(x, y, 0), picture(x, y, 1), picture(x, y, 2));
        bright(x, y, c) = max(0.0f, l - s(TV_ORTH_THRESHOLD));
        ms::interleaved(picture);
        settings.dim(0).set_bounds(0, TV_FIELDS);
        ms::interleaved(bright);
        ms::schedule_rgba(bright, get_target().has_gpu_feature());
    }
};

// The screen: the picture's damage and colour.
class TVDamageScreen : public Generator<TVDamageScreen> {
  public:
    Input<Buffer<float, 3>> picture{"picture"};
    Input<Buffer<float, 3>> narrow{"narrow"}, wide{"wide"}; // the bright parts blurred
    Input<Buffer<float, 2>> settings{"settings"};
    Output<Buffer<float, 3>> screen{"screen"};

    void generate() {
        Settings s{settings};
        Expr w = picture.dim(0).extent(), h = picture.dim(1).extent();
        Expr fw = cast<float>(w), fh = cast<float>(h);
        Expr seed = s.i(TV_SEED), frame = cast<int>(floor(s(TV_TIME)));
        Expr mono = s(TV_MONO) > 0.5f;
        Expr ch = select(mono, 0, c);
        Expr px = cast<float>(x) + 0.5f, py = cast<float>(y) + 0.5f;

        // The orthicon's halo.
        Expr halo = s(TV_ORTH_AMP) * max(0.0f, wide(x, y, 0) - narrow(x, y, 0));
        Expr v = picture(x, y, c) - halo;

        // Static: 0.437 x (h(x) + 0.6 h(x - 1)) has sd 0.294 (the reference's at density 1).
        Expr tz = frame * 7919 + seed;
        Expr noise = 0.437f * (symmetric(hash3(x, y, tz * 4 + ch)) +
                               0.6f * symmetric(hash3(x - 1, y, tz * 4 + ch)));
        Expr touched = uniform(hash3(x, y, tz * 4 + 3)) < s(TV_STATIC_SHARE);
        v += select(touched, s(TV_STATIC_AMP) * clamp(noise * s(TV_STATIC_BOOST), -1.25f, 1.25f),
                    0.0f);

        // Interference dots.
        Expr jitter = s(TV_DOTS_JITTER) * symmetric(hash3(y, frame, seed + 433));
        Expr cx = cast<int>(floor((px + s(TV_DOTS_X) + jitter) / s(TV_DOTS_W)));
        Expr cy = cast<int>(floor((py + s(TV_DOTS_Y)) / s(TV_DOTS_H)));
        Expr dot = uniform(hash3(cx, cy, seed + 2711)) < s(TV_DOTS_SHARE);
        Expr level = symmetric(hash3(cx, cy, seed + 2713 + ch));
        v += select(dot, s(TV_DOTS_AMP) * level * abs(level), 0.0f);

        // Colour stripes.
        Expr phase = s(TV_STRIPES_FX) * (px - 0.5f * fw) + s(TV_STRIPES_FY) * (py - 0.5f * fh) +
                     s(TV_STRIPES_PHASE) + select(c == 1, 1.0f / 3.0f, c == 2, -1.0f / 3.0f, 0.0f);
        v *= 1.0f + s(TV_STRIPES_AMP) * cos(TAU * phase);

        Func damaged("damaged");
        damaged(x, y, c) = select(c < 3, v, picture(x, y, c));

        // Colour correction: hue (HSV, luma kept), saturation, darks + max(lights - darks, 0) x c.
        Expr r = damaged(x, y, 0), g = damaged(x, y, 1), b = damaged(x, y, 2);
        Expr l0 = luma(r, g, b);
        Expr shifted = ms::hue_shifted(r, g, b, c, s(TV_HUE), damaged(x, y, c));
        Expr sat = select(mono, 0.0f, s(TV_SATURATION));
        Expr cc = l0 + sat * (shifted - l0);
        Expr darks = select(c == 0, s(TV_DARKS_R), c == 1, s(TV_DARKS_G), s(TV_DARKS_B));
        Expr lights = select(c == 0, s(TV_LIGHTS_R), c == 1, s(TV_LIGHTS_G), s(TV_LIGHTS_B));
        Expr out = darks + max(lights - darks, 0.0f) * cc;

        // Bars: (5/6 x envelope x carrier - 1/3) x amplitude.
        Expr p1 = py * s(TV_BARS_FREQ) / s(TV_BARS_PERIOD) - s(TV_BARS_PHASE1);
        p1 = p1 - floor(p1) - 0.5f;
        Expr width = max(s(TV_BARS_WIDTH), 1e-3f);
        Expr bump = select(abs(p1) < width, cos(0.5f * TAU * p1 / (2.0f * width)), 0.0f);
        Expr envelope = pow(bump * bump, 0.5f / max(s(TV_BARS_SHARP1), 0.01f));
        Expr p2 = py * s(TV_BARS_REL2) / s(TV_BARS_PERIOD) - s(TV_BARS_PHASE2);
        Expr carrier = pow(0.5f + 0.5f * cos(TAU * p2), 0.5f / max(s(TV_BARS_SHARP2), 0.01f));
        out += s(TV_BARS_AMP) * (5.0f / 6.0f * envelope * carrier - 1.0f / 3.0f);

        // Tape dropouts: white streaks on the rows where a slow noise (over the rows and time)
        // passes the threshold, on the frames Dropouts Always lets.
        Expr rowv = 0.55f + 0.4f * ms_noise::perlin3(py / fh * s(TV_DROP_FREQ) * 0.5f,
                                                     s(TV_TIME) * 0.15f,
                                                     cast<float>(seed % 977) + 0.37f);
        Expr period = s(TV_DROP_LEN) + s(TV_DROP_GAP);
        Expr along = px + uniform(hash3(y, frame, seed + 5011)) * period;
        Expr streak = rowv > s(TV_DROP_THRESHOLD) && uniform(hash3(y, frame, seed + 5017)) < 0.5f &&
                      along - period * floor(along / max(period, 1.0f)) <
                                                         s(TV_DROP_LEN);
        Expr this_frame = uniform(hash3(frame, seed, 5021)) < s(TV_DROP_CHANCE);
        out += select(streak && this_frame, s(TV_DROP_AMP), 0.0f);

        // Scanlines and vignette.
        out *= 1.0f + s(TV_SCAN_AMP) * sin(TAU * s(TV_SCAN_FREQ) * (py - 0.5f * fh));
        Expr dx = (px - 0.5f * fw) / (0.5f * fw);
        Expr dy = (py - 0.5f * fh) / (s(TV_VIG_REL_H) * 0.5f * fw);
        Expr dist = sqrt(dx * dx + dy * dy);
        Expr soft = max(s(TV_VIG_SOFT), 2.0f / fw); // (at least a pixel)
        Expr tt = clamp((dist - s(TV_VIG_RADIUS)) / soft + 0.5f, 0.0f, 1.0f);
        out *= 1.0f - s(TV_VIG_AMP) * (tt + 0.44f * tt * (1.0f - tt) * (2.0f * tt - 1.0f));

        screen(x, y, c) = select(c < 3, out, damaged(x, y, 3));

        ms::interleaved(picture);
        ms::interleaved(narrow);
        ms::interleaved(wide);
        settings.dim(0).set_bounds(0, TV_FIELDS);
        ms::interleaved(screen);
        const bool gpu = get_target().has_gpu_feature();
        damaged.compute_root();
        ms::schedule_rgba(damaged, gpu);
        ms::schedule_rgba(screen, gpu);
    }
};

// The picture's end: turn off, fish eye, TV pixels, the mask.
class TVDamageFinal : public Generator<TVDamageFinal> {
  public:
    Input<Buffer<float, 3>> screen{"screen"};
    Input<Buffer<float, 3>> cells{"cells"}; // TV Pixels' area means (else the screen)
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> mask_value{"mask_value"};
    Input<Buffer<float, 2>> settings{"settings"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Settings s{settings};
        Expr w = screen.dim(0).extent(), h = screen.dim(1).extent();
        Expr fw = cast<float>(w), fh = cast<float>(h);
        Expr cxp = 0.5f * fw, cyp = 0.5f * fh;
        Expr px = cast<float>(x) + 0.5f, py = cast<float>(y) + 0.5f;

        // Turn off. Phase 1 (up to 0.3): the picture in a box shrinking about the
        // centre (e^-6q of the frame), lit by Flare Brightness, trailing outwards along square
        // rings (s^-1.3, to black at the frame's edge). Phase 2: a line and a spot, fading out
        // from TV_OFF_FADE.
        Expr off = s(TV_OFF), fade = max(s(TV_OFF_FADE), 0.3f), squeeze = 0.3f;
        Expr q = clamp(off / squeeze, 0.0f, 1.0f);
        Expr later = clamp((off - fade) / max(1.0f - fade, 1e-3f), 0.0f, 1.0f);
        Expr dx = px - cxp, dy = py - cyp;
        Expr kx = max(exp(-6.0f * q), 0.5f / cxp), ky = max(kx * (1.0f - 0.5f * q), 0.5f / cyp);
        Expr ring = max(abs(dx) / (kx * cxp), abs(dy) / (ky * cyp));
        Expr ring1 = max(ring, 1.0f);
        Expr edge = ring * min(cxp / max(abs(dx), 1e-3f), cyp / max(abs(dy), 1e-3f));
        Expr toward = clamp((ring - 1.0f) / max(edge - 1.0f, 1e-3f), 0.0f, 1.0f);
        Expr trail = select(ring <= 1.0f, 1.0f, pow(ring1, -1.3f) * (1.0f - toward * toward));
        Expr squeezing = off > 0.0f && off < squeeze;
        // (the trails read the picture's border inside it)
        Expr qx = select(squeezing, clamp(cxp + dx / (ring1 * kx), 0.5f, fw - 0.5f), px);
        Expr qy = select(squeezing, clamp(cyp + dy / (ring1 * ky), 0.5f, fh - 0.5f), py);

        // Fish eye.
        Expr u = (qx - cxp) / cxp, v = (qy - cyp) / cxp, k = s(TV_FISH);
        Expr r2 = u * u + v * v;
        Expr f = select(k > 0.0f, (1.0f + k * r2) / (1.0f + k), 1.0f + k * r2);
        Expr sx = cxp + u * f * cxp, sy = cyp + v * f * cxp;

        // TV pixels: the cell the point falls in.
        Expr cw = cells.dim(0).extent(), chh = cells.dim(1).extent();
        Expr cx = clamp(cast<int>(floor(sx / fw * cast<float>(cw))), 0, cw - 1);
        Expr cy = clamp(cast<int>(floor(sy / fh * cast<float>(chh))), 0, chh - 1);
        Expr outside = sx < 0.0f || sx >= fw || sy < 0.0f || sy >= fh;
        Func screen_in("screen_in");
        screen_in(x, y, c) = screen(x, y, c);
        Expr seen = select(s(TV_CELLS) > 0.5f, select(outside, 0.0f, cells(cx, cy, c)),
                           bilinear(screen_in, sx, sy, w, h));

        // Turn off's phase 2: a one-row line (falling off towards the sides) and a spot
        // (exponential), both times Flare Brightness and the picture's centre.
        Expr unit = fw / 320.0f;
        Expr spot_x = max(0.163f * s(TV_OFF_WIDTH), unit), spot_y = 4.5f * unit;
        Expr spot = exp(-sqrt((dx / spot_x) * (dx / spot_x) + (dy / spot_y) * (dy / spot_y)));
        Expr on_line = y == h / 2;
        Expr line = select(on_line, max(0.0f, 1.0f - abs(dx) / (0.75f * fw)), 0.0f);
        Expr centre = screen(clamp(w / 2, 0, w - 1), clamp(h / 2, 0, h - 1), c);
        Expr phase2 =
            centre * s(TV_OFF_BRIGHT) * (3.13f * spot + 1.065f * line) * (1.0f - later);
        Expr shown =
            select(off <= 0.0f, seen, off < squeeze, seen * s(TV_OFF_BRIGHT) * trail, phase2);
        shown = select(c == 3, select(off >= 1.0f, 0.0f, seen), shown);

        Expr src = source(x, y, c);
        output(x, y, c) = src + (shown - src) * mask_value(x, y);

        ms::interleaved(screen);
        ms::interleaved(cells);
        ms::interleaved(source);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        settings.dim(0).set_bounds(0, TV_FIELDS);
        ms::interleaved(output);
        ms::schedule_rgba(output, get_target().has_gpu_feature());
    }
};

HALIDE_REGISTER_GENERATOR(TVDamageSource, ms_tv_damage_source)
HALIDE_REGISTER_GENERATOR(TVDamageBright, ms_tv_damage_bright)
HALIDE_REGISTER_GENERATOR(TVDamageScreen, ms_tv_damage_screen)
HALIDE_REGISTER_GENERATOR(TVDamageFinal, ms_tv_damage_final)
