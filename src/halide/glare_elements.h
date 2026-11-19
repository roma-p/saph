// The elements of the glare effects (Glare, Lens Flare): one element's value at an offset
// from its centre, from its row of the style (glare_layout.h).

#pragma once

#include <functional>

#include "Halide.h"
#include "glare_layout.h"
#include "noise.h"

namespace ms_glare {

using namespace Halide;

const float PI = 3.14159265358979f;

inline Expr smoothstep(Expr t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// erf(x), Abramowitz and Stegun 7.1.26 (error 1.5e-7).
inline Expr erf_approx(Expr v) {
    Expr a = abs(v);
    Expr t = 1.0f / (1.0f + 0.3275911f * a);
    Expr p = ((((1.061405429f * t - 1.453152027f) * t + 1.421413741f) * t - 0.284496736f) * t +
              0.254829592f) *
             t;
    Expr r = 1.0f - p * exp(-a * a);
    return select(v < 0.0f, -r, r);
}

// Lens Flare's waves and noise: v (0..1) thresholded by a width w (1: all of it) and a
// softness s (0: hard), w + (v + w - 1) / s, within 0..1.
inline Expr threshold(Expr v, Expr w, Expr s) {
    return select(s > 0.0f, clamp(w + (v + w - 1.0f) / select(s > 0.0f, s, 1.0f), 0.0f, 1.0f),
                  select(v + w - 1.0f > 0.0f, 1.0f, 0.0f));
}

// A band's edge at distance d (pixels) outside it.
inline Expr band_edge(Expr d) { return 1.0f - smoothstep((d + 0.35f) / 2.2f); }

// Field i of element e (both may be computed; read clamped: Halide's bounds of computed
// fields would be unbounded).
using Field = std::function<Expr(Expr, Expr)>;

// Lens Flare's noises around element e at angle a (radians, -pi..pi): which 0, the radius
// noise (0..1), which 1, noise_brightness's thresholded noise (lensflare_generator.cpp).
using Angular = std::function<Expr(int, Expr, Expr)>;

// The noises around noisy elements (MillSaph's, lensflare_style.c ms_lens_polar_noises):
// NOISE_ANGLES angles a turn, from -pi, for each noise slot e of `nf` (a row of fields); which
// 0, the radius noise (0..1), which 1, noise_brightness's, thresholded and averaged over
// blur_theta_map x a turn (NOISE_TAPS angles). `angular` reads them for an element, its slot
// `slot_of(element)`, linearly between the angles; noises and thresholded to schedule.
const int NOISE_ANGLES = 4096, NOISE_TAPS = 24;
struct PolarNoises {
    Var i{"i"}, e{"e"}, which{"which"};
    Func raw{"raw"}, thresholded{"thresholded"}, noises{"noises"};
    Angular angular;
};
inline void polar_noises(PolarNoises &p, const Field &nf, std::function<Expr(Expr)> slot_of) {
    const int ANGLES = NOISE_ANGLES, TAPS = NOISE_TAPS;
    Var i = p.i, e = p.e, which = p.which;
    Expr at = -PI + (cast<float>(i) + 0.5f) * (2.0f * PI / ANGLES);
    auto around = [&](int rho, int seed, int octaves, int scale, float mean, float shift) {
        Expr n = ms_noise::fbm(cos(at) * nf(rho, e) + shift, sin(at) * nf(rho, e) + 3.3f,
                               nf(seed, e), nf(octaves, e), 0.43f, 8,
                               "octave" + std::to_string(rho));
        return clamp(mean + nf(scale, e) * n, 0.0f, 1.0f);
    };
    p.raw(i, e, which) = select(which == 0,
                                around(GLARE_RN_RHO, GLARE_RN_SEED, GLARE_RN_OCTAVES,
                                       GLARE_RN_SCALE, 0.47f, 11.1f),
                                around(GLARE_NB_RHO, GLARE_NB_SEED, GLARE_NB_OCTAVES,
                                       GLARE_NB_SCALE, 0.5f, 7.7f));
    p.thresholded(i, e) =
        threshold(p.raw(i, e, 1), nf(GLARE_NB_WIDTH, e), nf(GLARE_NB_SOFT, e));
    RDom tap(0, TAPS, "tap");
    Expr blur = nf(GLARE_NB_BLUR, e);
    Expr step = cast<int>(round(blur * ANGLES * ((cast<float>(tap) + 0.5f) / TAPS - 0.5f)));
    Expr averaged =
        select(blur > 0.0f, sum(p.thresholded((i + step + 4 * ANGLES) % ANGLES, e)) / TAPS,
               p.thresholded(i, e));
    p.noises(i, e, which) = select(which == 0, p.raw(i, e, 0), averaged);
    Func noises = p.noises;
    p.angular = [noises, slot_of](int k, Expr a, Expr el) {
        Func n = noises;
        Expr t = (a + PI) * (NOISE_ANGLES / (2.0f * PI)) - 0.5f;
        Expr t0 = floor(t), w = t - t0;
        Expr i0 = (cast<int>(t0) + NOISE_ANGLES) % NOISE_ANGLES, i1 = (i0 + 1) % NOISE_ANGLES;
        Expr row = clamp(slot_of(el), 0, GLARE_MAX_ELEMENTS - 1);
        return n(i0, row, k) * (1.0f - w) + n(i1, row, k) * w;
    };
}

// Element e's stops_profile at radius r, channel c, read from a table (Lens Flare,
// lensflare_generator.cpp): the same profile, interpolated.
using Profile = std::function<Expr(Expr, Expr, Expr)>;

// Element e's frame at offset (dx, dy) (pixels, y up) from its centre: the offset scaled,
// turned and stretched (ex, ey: r0, th), its angle without the stretch (sectors, waves, the
// rays' width), the radius its band is cut at.
struct ElementFrame {
    Expr rot, cr, sr, turned;
    Expr sx, sy, qx, qy; // scaled by the stretch (Glare); by rel_width, rel_height (Lens Flare)
    Expr ax, ay;         // scaled without the stretch
    Expr r0, th, angle, clip_r;
};

inline ElementFrame element_frame(const Field &field, Expr dx, Expr dy, Expr e) {
    auto f = [&](int i) { return field(i, e); };
    ElementFrame m;
    Expr kind = f(GLARE_KIND);
    m.rot = f(GLARE_ROTATE);
    m.sx = dx / f(GLARE_SCALE_X), m.sy = dy / f(GLARE_SCALE_Y);
    // (Lens Flare: scaled by rel_width, rel_height in the screen's frame (q), then turned by
    // its rotation and stretched in that frame)
    m.turned = f(GLARE_TURNED_FRAME) == 1.0f;
    m.cr = cos(m.rot), m.sr = sin(m.rot);
    m.qx = dx / max(f(GLARE_REL_X), 1e-6f), m.qy = dy / max(f(GLARE_REL_Y), 1e-6f);
    Expr tx = m.qx * m.cr + m.qy * m.sr, ty = -m.qx * m.sr + m.qy * m.cr;
    // (Glare's renderer 8 (2): Rel Width / Height along x, y, turned, then the stretch: the
    // angle frame's point over the stretch alone, SCALE / ANGLE)
    Expr spun = f(GLARE_TURNED_FRAME) == 2.0f;
    Expr gx = dx / f(GLARE_ANGLE_X), gy = dy / f(GLARE_ANGLE_Y);
    Expr sx2 = (gx * m.cr + gy * m.sr) * f(GLARE_ANGLE_X) / f(GLARE_SCALE_X);
    Expr sy2 = (-gx * m.sr + gy * m.cr) * f(GLARE_ANGLE_Y) / f(GLARE_SCALE_Y);
    Expr ex = select(m.turned, tx / f(GLARE_SCALE_X), spun, sx2, m.sx * m.cr + m.sy * m.sr);
    Expr ey = select(m.turned, ty / f(GLARE_SCALE_Y), spun, sy2, -m.sx * m.sr + m.sy * m.cr);
    // (Lens Flare, stretch_distort of strength k: the shape (not its image), in units of R,
    // from y to y + k (0.213 - 0.311 y + 0.515 y^2), x x (1 + 0.472 k y); fitted at 7
    // stretches and strengths, 1-5 % of the pixels)
    Expr dk = f(GLARE_DISTORT), Rd = max(f(GLARE_R), 1e-6f);
    Expr qa = dk * 0.515f, qb = 1.0f - dk * 0.311f, qc = dk * 0.213f - ey / Rd;
    Expr root = -2.0f * qc / (qb + sqrt(max(qb * qb - 4.0f * qa * qc, 0.0f)));
    ex = select(dk != 0.0f, ex / (1.0f + dk * 0.472f * root), ex);
    ey = select(dk != 0.0f, root * Rd, ey);
    m.r0 = sqrt(ex * ex + ey * ey);
    m.th = atan2(ey, ex);
    // The angles of sectors and waves, and the rays' width: without the stretch.
    m.ax = dx / f(GLARE_ANGLE_X), m.ay = dy / f(GLARE_ANGLE_Y);
    Expr aex = select(m.turned, tx / f(GLARE_ANGLE_X), m.ax * m.cr + m.ay * m.sr);
    Expr aey = select(m.turned, ty / f(GLARE_ANGLE_Y), -m.ax * m.sr + m.ay * m.cr);
    m.angle = atan2(aey, aex);
    m.clip_r = select(m.turned, select(kind == GLARE_RAYS, sqrt(m.qx * m.qx + m.qy * m.qy), m.r0),
                      sqrt(dx * dx + dy * dy) / f(GLARE_CLIP_SCALE));
    return m;
}

// FALLOFF: (1 - t)^1.965 exp(-0.475 t), inside its circle.
inline Expr falloff_value(const Field &field, const ElementFrame &m, Expr e) {
    Expr tb = m.r0 / field(GLARE_FALLOFF_R, e);
    return pow(max(1.0f - tb, 0.0f), 1.965f) * exp(-0.475f * tb) *
           band_edge(m.clip_r - field(GLARE_CLIP_R, e));
}

// STOPS: the radius through the polygon (regular, or Lens Flare's irregular ones with the
// noisy elements), the radius waves, and Lens Flare's radius noise.
inline Expr stops_radius(const Field &field, const ElementFrame &m, Expr e, const Angular *noise) {
    auto f = [&](int i) { return field(i, e); };
    Expr start = f(GLARE_START);
    Expr sides = f(GLARE_SIDES);
    Expr seg = 2.0f * PI / max(sides, 3.0f);
    Expr phase = m.th - start;
    Expr phi = phase - seg * floor(phase / seg) - seg / 2.0f;
    Expr edge = cos(PI / max(sides, 3.0f)) / cos(phi);
    if (noise) {
        // (Lens Flare's irregular polygons, with the noisy elements: the side the angle is on,
        // the last vertex before it by halving, its half angle and middle)
        Expr turn_at = phase - 2.0f * PI * floor(phase / (2.0f * PI));
        Expr side = 0;
        for (int step = GLARE_MAX_SIDES / 2; step >= 1; step /= 2)
            side = select(field(GLARE_VERTICES + side + step, e) <= turn_at, side + step, side);
        Expr v0 = field(GLARE_VERTICES + side, e), v1 = field(GLARE_VERTICES + side + 1, e);
        edge = select(f(GLARE_IRREGULAR) != 0.0f,
                      cos(0.5f * (v1 - v0)) / cos(turn_at - 0.5f * (v0 + v1)), edge);
    }
    // (a strongly negative roundness: thin spikes at the vertices, nothing between them; the
    // edge kept above 0)
    edge = max(edge + (1.0f - edge) * f(GLARE_ROUNDNESS), 1e-4f);
    Expr r1 = select(sides >= 3.0f, m.r0 / edge, m.r0);
    Expr lo = f(GLARE_RW_MIN), hi = f(GLARE_RW_MAX);
    // (their frequency across the sector's width; a whole turn: per turn)
    Expr whole = phase - 2.0f * PI * floor(phase / (2.0f * PI));
    Expr wave_c =
        0.5f + 0.5f * cos(f(GLARE_RW_FREQ) * whole * (2.0f * PI / f(GLARE_WIDTH)));
    Expr r = select(lo != 1.0f || hi != 1.0f,
                    r1 / (lo + (hi - lo) * pow(max(wave_c, 0.0f), f(GLARE_RW_SPIKE))), r1);
    if (noise) {
        // (Lens Flare: the radius x r_noise_min..max by the radius noise)
        Expr lo_n = f(GLARE_RN_MIN), hi_n = f(GLARE_RN_MAX);
        r = select(f(GLARE_RN_OCTAVES) > 0.0f,
                   r / max(lo_n + (hi_n - lo_n) * (*noise)(0, m.th, e), 1e-3f), r);
    }
    return r;
}

// STOPS: the colour at radius r: linear between the stops (held beyond them), in its band
// (edges as the row says), or blurred (Blur Gradient); then faded (fade_gradient).
inline Expr stops_profile(const Field &field, Expr r, Expr c, Expr e) {
    auto f = [&](int i) { return field(i, e); };
    Expr R = f(GLARE_R);
    Expr t = r / R;
    Expr stops = f(GLARE_STOP_COUNT);
    auto radius = [&](Expr k) { return field(GLARE_STOP_RADII + k, e); };
    auto colour = [&](Expr k) { return field(GLARE_STOP_COLORS + 3 * k + c, e); };
    Expr last = cast<int>(stops) - 1;
    RDom k(0, GLARE_MAX_STOPS - 1, "k");
    k.where(k < last);
    Expr ka = radius(k), kb = radius(k + 1);
    Expr inside = t >= ka && t < kb && kb > ka;
    Expr between = sum(select(inside, colour(k) + (colour(k + 1) - colour(k)) *
                                                      ((t - ka) / select(kb > ka, kb - ka, 1.0f)),
                              0.0f));
    Expr held = select(t < radius(0), colour(0), t >= radius(last), colour(last), between);
    auto edge_of = [&](Expr d) {
        return select(f(GLARE_CRISP) == 2.0f, clamp(0.5f - (d - 0.21f) / 1.18f, 0.0f, 1.0f),
                      f(GLARE_CRISP) != 0.0f, clamp(0.57f - d, 0.0f, 1.0f), band_edge(d));
    };
    Expr outer = edge_of(r - radius(last) * R);
    Expr inner = select(radius(0) > 0.0f, edge_of(radius(0) * R - r), 1.0f);
    // Blur Gradient: that profile (0 outside the band) blurred by a gaussian of sigma: its
    // jumps through Phi, its slope changes through x Phi(x / sigma) + sigma phi(x / sigma).
    Expr sigma = f(GLARE_SIGMA), sig = select(sigma > 0.0f, sigma, 1.0f);
    auto Phi = [&](Expr d) { return 0.5f * (1.0f + erf_approx(d / (sig * 1.41421356f))); };
    auto ramp = [&](Expr d) {
        return d * Phi(d) + sig * exp(-0.5f * (d / sig) * (d / sig)) * 0.39894228f;
    };
    auto knot = [&](Expr i) { return radius(i) * R; };
    auto slope = [&](Expr i) { // of the segment from stop i to i + 1
        Expr gap = knot(i + 1) - knot(i);
        Expr rise = (colour(i + 1) - colour(i)) / select(gap > 0.0f, gap, 1.0f);
        return select(i >= 0 && gap > 0.0f, rise, 0.0f);
    };
    RDom q(0, GLARE_MAX_STOPS - 1, "q");
    q.where(q < last);
    Expr bends = sum((slope(q) - slope(q - 1)) * ramp(r - knot(q)));
    Expr blurred = select(radius(0) > 0.0f, colour(0) * Phi(r - knot(0)), colour(0)) + bends -
                   slope(last - 1) * ramp(r - knot(last)) - colour(last) * Phi(r - knot(last));
    Expr disc = select(r <= radius(0) * R, colour(0), 0.0f); // one stop
    Expr col = select(stops < 2.0f, disc, sigma > 0.0f, blurred, held * outer * inner);
    // fade_gradient f: x 1 - f t outwards; f < 0: x 1 + f (1 - t), darker at the centre
    // (Lens Flare's measures at -0.75, -0.3, 0.5, 0.89)
    Expr fade = f(GLARE_FADE), tc = clamp(t, 0.0f, 1.0f);
    return col * select(fade < 0.0f, 1.0f + fade * (1.0f - tc), 1.0f - fade * tc);
}

// Sectors: the angular window (repeated), soft edges; its position in the window (u: 0..1
// across it, v: in its repeat) for the waves.
inline Expr sector_window(const Field &field, const ElementFrame &m, Expr e, Expr &u, Expr &v) {
    auto f = [&](int i) { return field(i, e); };
    Expr width = f(GLARE_WIDTH), soft = f(GLARE_SOFTNESS);
    Expr aphase = m.angle - f(GLARE_START);
    Expr turn = aphase - 2.0f * PI * floor(aphase / (2.0f * PI));
    u = turn / width;
    v = u * f(GLARE_REPEATS) - floor(u * f(GLARE_REPEATS));
    // Sharp edges (no softness): the start edge itself left out, the end edge over one
    // pixel of arc.
    Expr past = (turn - width) * m.r0;
    Expr sharp = select(u <= 1e-5f, 0.0f, u <= 1.0f, 1.0f, clamp(0.57f - past, 0.0f, 1.0f));
    Expr window = select(soft > 0.0f,
                         smoothstep(min(v, 1.0f - v) / (0.5f * soft)) *
                             select(u <= 1.0f, 1.0f, 0.0f),
                         sharp);
    return select(width >= 2.0f * PI - 1e-5f && soft == 0.0f, 1.0f, window);
}

// A polar flare's waves at (u, v) of its sector (sector_window).
inline Expr sector_waves(const Field &field, Expr e, Expr u, Expr v) {
    auto f = [&](int i) { return field(i, e); };
    Expr wave = f(GLARE_WAVE), ww = f(GLARE_WAVE_WIDTH);
    Expr ph = (u - 0.5f) * f(GLARE_WAVE_FREQ) + 0.5f;
    ph = ph - floor(ph) - 0.5f;
    Expr pulse = 1.0f - smoothstep(abs(ph) / (0.5f * select(ww > 0.0f, ww, 1.0f)));
    Expr waves = select(wave == 0.0f, 1.0f, ww >= 1.0f, 1.0f + wave, 1.0f + wave * pulse);
    // (Lens Flare: x wave_brightness x the cosine wave thresholded by width and softness,
    // a peak at the sector's start, shifted by wave_phase)
    // (in each of the sector's repeats)
    Expr cosine =
        0.5f + 0.5f * cos(2.0f * PI * (v * f(GLARE_WAVE_FREQ) - f(GLARE_WAVE_PHASE)));
    return select(f(GLARE_WAVE_LENS) == 0.0f || wave == 0.0f, waves,
                  wave * threshold(cosine, ww, f(GLARE_WAVE_SOFT)));
}

// RAYS: lines through the centre, 1 - d / L along (stretched), 1 - smoothstep(d / w) across
// (not stretched); cut at L unstretched.
inline Expr rays_value(const Field &field, const ElementFrame &m, Expr dx, Expr dy, Expr e) {
    auto f = [&](int i) { return field(i, e); };
    Expr L = f(GLARE_R), w = f(GLARE_RAY_WIDTH), lines = f(GLARE_LINES);
    RDom j(0, GLARE_MAX_LINES, "j");
    j.where(cast<float>(j) < lines);
    // (each line's length and turn: MillSaph's random by the line and the seed)
    Expr seed = cast<int>(f(GLARE_RAY_SEED) * 1000.0f);
    auto rnd = [&](int k) {
        return cast<float>(ms_noise::hash3(j, seed, k) & ms_noise::u32(0xffffff)) / 16777216.0f;
    };
    Expr lmin = f(GLARE_RAY_MIN);
    Expr Lj = L * select(lmin < 1.0f, lmin + (1.0f - lmin) * sqrt(rnd(1)), 1.0f);
    Expr spacing = PI / select(lines > 0.0f, lines, 1.0f);
    Expr ang = m.rot + cast<float>(j) * spacing + f(GLARE_RAY_JITTER) * (rnd(2) - 0.5f) * spacing;
    Expr ca = cos(ang), sa = sin(ang); // (Lens Flare's frame q: the screen's, the same angle)
    // (Lens Flare: each line turned by the rotation in the frame q, its length there; its
    // width across it as the squeeze makes it: x rel_width rel_height / |v|)
    Expr vx = f(GLARE_REL_X) * ca, vy = f(GLARE_REL_Y) * sa;
    Expr vlen = max(sqrt(vx * vx + vy * vy), 1e-6f);
    Expr along_q = (dx * vx + dy * vy) / (vlen * vlen);
    // (on the screen: x rel_height cos^2 + rel_width sin^2 of the line's angle there (fitted
    // at 4 angles), at least 0.75 px wide, as bright in all)
    Expr squeezed = w * (f(GLARE_REL_Y) * ca * ca + f(GLARE_REL_X) * sa * sa);
    Expr shown = max(squeezed, 0.75f);
    Expr across_s = (-dx * vy + dy * vx) / vlen;
    Expr along = select(m.turned, along_q, m.sx * ca + m.sy * sa);
    Expr across = select(m.turned, across_s, -m.ax * sa + m.ay * ca);
    Expr ray_w = select(m.turned, shown, w), thin = select(m.turned, squeezed / shown, 1.0f);
    Expr safe_w = select(ray_w > 0.0f, ray_w, 1.0f); // no width: no rays
    return select(w > 0.0f,
                  sum(max(1.0f - abs(along) / Lj, 0.0f) *
                      (1.0f - smoothstep(abs(across) / safe_w)) * thin),
                  0.0f) *
           band_edge(m.clip_r - f(GLARE_CLIP_R));
}

// Element e's value, channel c, at offset (dx, dy) (pixels, y up) from its centre; noise:
// with Lens Flare's radius noise and noise_brightness (the elements without them are
// cheaper without); image: Lens Flare's image there (it multiplies the element, or is it);
// profile: its stops_profile from a table (else computed here).
inline Expr element_value(const Field &field, Expr dx, Expr dy, Expr c, Expr e,
                          const Angular *noise = nullptr, Expr image = Expr(),
                          const Profile *profile = nullptr) {
    auto f = [&](int i) { return field(i, e); };
    Expr kind = f(GLARE_KIND);
    const ElementFrame m = element_frame(field, dx, dy, e);
    Expr gain = select(c == 0, f(GLARE_GAIN_R), c == 1, f(GLARE_GAIN_G), f(GLARE_GAIN_B));
    Expr falloff = falloff_value(field, m, e);
    Expr r = stops_radius(field, m, e, noise);
    Expr col = profile ? (*profile)(r, c, e) : stops_profile(field, r, c, e);
    Expr u, v;
    Expr window = sector_window(field, m, e, u, v);
    Expr waves = sector_waves(field, e, u, v);
    Expr gradient = col * select(f(GLARE_SECTOR) != 0.0f, window, 1.0f) * f(GLARE_BOOST) * waves;
    if (noise) // (Lens Flare: x noise_brightness x its noise)
        gradient = select(f(GLARE_NB) != 0.0f, gradient * f(GLARE_NB) * (*noise)(1, m.th, e),
                          gradient);
    Expr ray = rays_value(field, m, dx, dy, e);
    Expr value = select(kind == GLARE_FALLOFF, falloff, kind == GLARE_RAYS, ray, gradient);
    if (image.defined())
        value = select(kind == GLARE_IMAGE, image, value * image);
    return value * gain;
}

// The radius past which element e's stops_profile is 0 (Blur Gradient: nearly): its last
// stop, at least R, + its edge (2 pixels at most) and 5 sigma of the blur.
inline Expr profile_end(const Field &field, Expr e) {
    Expr last = max(cast<int>(field(GLARE_STOP_COUNT, e)) - 1, 0);
    Expr R = field(GLARE_R, e);
    return R * max(field(GLARE_STOP_RADII + last, e), 1.0f) + 5.0f * field(GLARE_SIGMA, e) + 4.0f;
}

// How far element e reaches (pixels): its radius x its stretch, x 1.6 for the radius waves
// and blurred edges.
inline Expr element_reach(const Field &field, Expr e) {
    Expr rel = max(max(field(GLARE_REL_X, e), field(GLARE_REL_Y, e)), 1.0f); // (Lens Flare)
    return field(GLARE_R, e) * 1.6f * max(field(GLARE_SCALE_X, e), field(GLARE_SCALE_Y, e)) * rel +
           4.0f * field(GLARE_SIGMA, e) + 4.0f;
}

} // namespace ms_glare
