// MS_LensFlare in Halide (effects/ms_lensflare.c runs the kernels; the elements are Glare's,
// glare_elements.h, each at its own place along the flare's axis):
//   ms_lensflare_occlusion  how much of the flare shows: 1 - f^2, f the Occlusion input's
//                           mean alpha (or luma) over a square around the hotspot (by
//                           Occlusion Softness), one value in pixel (0, 0)
//   ms_lensflare_draw       the flare: the sum of the style's elements (glare_layout.h) at
//                           each pixel, each where it reaches (those that take it x the
//                           Atmosphere, a noise over the frame), x the occlusion's value;
//                           each row's radial profile first, in a table read at each pixel
//   ms_lensflare_glow       the Diffraction Glow added: the Occlusion input's edges near the
//                           hotspot, blurred, x how much of the flare shows
//   ms_lensflare_texture    one of MillSaph's own textures (lens dirt, a prism glint, a
//                           rainbow ramp, an iris), generated before the rest (on the CPU)
//   ms_lensflare_finish     Brightness x Color, Hue Shift, Saturation, Blur Flare, then
//                           combined with the Source x Bg Brightness (Screen, Add or the flare
//                           alone), Tint Bg Whites, Affect Alpha
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first (y up).

#include "Halide.h"
#include "blur.h"
#include "glare_elements.h"
#include "glare_layout.h"
#include "hue.h"
#include "noise.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c"), e("e");

// The schedule of a whole-image stage: float4 pixels, tiles of threads.
void tiles(Func f, bool gpu, int channels = 4) {
    Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
    f.bound(c, 0, channels).reorder(c, x, y);
    if (gpu && channels > 1)
        f.vectorize(c);
    if (gpu)
        f.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
    else
        f.unroll(c).vectorize(x, 8).parallel(y);
}

Expr luma(Expr r, Expr g, Expr b) { return 0.3125f * r + 0.5f * g + 0.1875f * b; }

} // namespace

class LensFlareOcclusion : public Generator<LensFlareOcclusion> {
  public:
    Input<Buffer<float, 3>> occluder{"occluder"};
    Input<int> from{"from"};     // 0: none, 1: luma, 2: alpha
    Input<int> invert{"invert"}; // its black areas occlude
    Input<int> hotspot_x{"hotspot_x"}, hotspot_y{"hotspot_y"}; // the hotspot's pixel, y up
    Input<int> reach{"reach"}; // the square's half side (pixels)
    Output<Buffer<float, 3>> visible{"visible"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        // f: the mean of the occluder (0..1, 0 off the image) over the square of side
        // 2 reach + 1 around the hotspot's pixel; visible 1 - f^2. Column sums first (one
        // thread each on the GPU), then their sum.
        Var i("i");
        Expr ix = hotspot_x + i;
        RDom j(-reach, 2 * reach + 1, "j");
        Expr iy = hotspot_y + j;
        Expr on = ix >= 0 && ix < occluder.width() && iy >= 0 && iy < occluder.height();
        auto at = [&](int ch) {
            return occluder(clamp(ix, 0, occluder.width() - 1), clamp(iy, 0, occluder.height() - 1),
                            ch);
        };
        Expr a = select(from == 1, luma(at(0), at(1), at(2)), at(3));
        a = clamp(a, 0.0f, 1.0f);
        a = select(invert != 0, 1.0f - a, a);
        Func column("column");
        column(i) = sum(select(on, a, 0.0f));
        RDom k(-reach, 2 * reach + 1, "k");
        Expr side = cast<float>(2 * reach + 1);
        Expr f = sum(column(k)) / (side * side);
        visible(x, y, c) = select(from == 0, 1.0f, clamp(1.0f - f * f, 0.0f, 1.0f));

        ms::interleaved(occluder);
        ms::interleaved(visible);
        visible.bound(x, 0, 1).bound(y, 0, 1).bound(c, 0, 4);
        column.compute_root();
        if (gpu) {
            Var io("io"), ii("ii");
            column.gpu_tile(i, io, ii, 64);
            visible.gpu_single_thread();
        } else {
            column.parallel(i, 64);
        }
    }
};

class LensFlareDraw : public Generator<LensFlareDraw> {
  public:
    Input<Buffer<float, 2>> rows{"rows"}; // (field, element), glare_layout.h
    // The noisy elements' rows (GLARE_MAX_ELEMENTS at most): their noises, computed once for
    // every row whose GLARE_NOISE_SLOT names one
    Input<Buffer<float, 2>> noise_rows{"noise_rows"};
    // The rows drawn, in lists by how (glare_layout.h GLARE_DRAW_*, one after the other from
    // row 0): how many in each
    Input<int> plain{"plain"}, shadowed_count{"shadowed"}, special_count{"special"},
        both{"special_shadowed"};
    Input<Buffer<float, 2>> texels{"texels"}; // the images' (glare_layout.h GLARE_TEXEL_ROW)
    Input<Buffer<float, 3>> visible{"visible"}; // ms_lensflare_occlusion's value
    // The rows in use (the lists, then the shadows' rows), and the samples of each one's
    // profile in the table
    Input<int> count{"count"}, samples{"samples"};
    // The Atmosphere: 1 + amp x a noise (fbm of `octaves`, frequency `freq` per pixel,
    // `persistence`, at time `z`), at least 0
    Input<float> amp{"amp"}, freq{"freq"}, persistence{"persistence"}, z{"z"};
    Input<int> octaves{"octaves"};
    // Occlusion Scale Brightness and Widths, minus 1
    Input<float> occluded_bright{"occluded_bright"}, occluded_width{"occluded_width"};
    Output<Buffer<float, 3>> flare{"flare"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func atmosphere("atmosphere");
        Expr n = ms_noise::fbm((cast<float>(x) + 0.5f) * freq, (cast<float>(y) + 0.5f) * freq, z,
                               octaves, persistence, 12, "octave");
        atmosphere(x, y) = max(1.0f + amp * n, 0.0f);
        ms_glare::Field field = [&](Expr i, Expr el) {
            return rows(clamp(i, 0, GLARE_FIELDS - 1), clamp(el, 0, GLARE_MAX_ROWS - 1));
        };
        auto noise_field = [&](Expr i, Expr slot) {
            return noise_rows(clamp(i, 0, GLARE_FIELDS - 1),
                              clamp(slot, 0, noise_rows.dim(1).extent() - 1));
        };
        // Each row's stops_profile (ms_glare), sampled from radius 0 to where it ends, read
        // linearly between its samples (the stops' loops once a row, not at each pixel).
        Var k("sample");
        Func profile("profile");
        auto spacing = [&](Expr el) {
            return max(ms_glare::profile_end(field, el), 1.0f) / cast<float>(samples - 1);
        };
        profile(k, c, e) = ms_glare::stops_profile(field, cast<float>(k) * spacing(e), c, e);
        ms_glare::Profile lookup = [&](Expr r, Expr ch, Expr el) {
            el = clamp(el, 0, count - 1);
            ch = clamp(ch, 0, 2);
            Expr u = clamp(r / spacing(el), 0.0f, cast<float>(samples - 1));
            Expr i0 = clamp(cast<int>(floor(u)), 0, samples - 2), w = u - cast<float>(i0);
            return profile(i0, ch, el) * (1.0f - w) + profile(i0 + 1, ch, el) * w;
        };
        // Occlusion Scale Brightness and Widths: each element brighter and wider as the
        // flare is hidden (w = 2 x the hidden part, at most 1), as much as it responds to
        // the triggers.
        Expr hidden = min(2.0f * (1.0f - visible(0, 0, 0)), 1.0f);
        auto wider = [&](Expr el) {
            return max(1.0f + occluded_width * field(GLARE_WIDTH_RESPONSE, el) * hidden, 1e-3f);
        };
        Expr brighter = 1.0f + occluded_bright * field(GLARE_BRIGHT_RESPONSE, e) * hidden;
        // The offset of the pixel's centre from element e's (in its widened frame).
        Expr dx = (cast<float>(x) + 0.5f - field(GLARE_POS_X, e)) / wider(e);
        Expr dy = (cast<float>(y) + 0.5f - field(GLARE_POS_Y, e)) / wider(e);
        // Masked by its shadow (another row, glare_style.c add_shadow), if it has one.
        Expr shadow_row = cast<int>(field(GLARE_SHADOW_ROW, e)) - 1;
        Expr sx = cast<float>(x) + 0.5f - field(GLARE_POS_X, shadow_row);
        Expr sy = cast<float>(y) + 0.5f - field(GLARE_POS_Y, shadow_row);
        Expr shadow = clamp(
            ms_glare::element_value(field, sx, sy, c, shadow_row, nullptr, Expr(), &lookup), 0.0f,
            1.0f);
        shadow = select(field(GLARE_SHADOW_INVERT, e) != 0.0f, 1.0f - shadow, shadow);
        Expr mask = 1.0f - field(GLARE_SHADOW_OPACITY, e) * (1.0f - shadow);
        Func element("element"), shadowed("shadowed");
        Expr atmo = select(field(GLARE_ATMOSPHERE, e) != 0.0f, atmosphere(x, y), 1.0f);
        Func noisy("noisy");
        element(x, y, c, e) =
            ms_glare::element_value(field, dx, dy, c, e, nullptr, Expr(), &lookup) * atmo *
            brighter;
        shadowed(x, y, c, e) = element(x, y, c, e) * mask;
        // The noises around each noisy element (glare_elements.h), one slot per noise row.
        ms_glare::PolarNoises pn;
        ms_glare::polar_noises(pn, noise_field, [&](Expr el) {
            return cast<int>(field(GLARE_NOISE_SLOT, el));
        });
        const int ANGLES = ms_glare::NOISE_ANGLES;
        Var i = pn.i, which = pn.which;
        Func thresholded = pn.thresholded, noises = pn.noises;
        ms_glare::Angular angular = pn.angular;
        // The element's image (if any) at the pixel: bilinear between its texels, 0 off it.
        Expr tx = cast<float>(x) + 0.5f - field(GLARE_TEX_X, e);
        Expr ty = cast<float>(y) + 0.5f - field(GLARE_TEX_Y, e);
        Expr tw = field(GLARE_TEX_W, e), th = field(GLARE_TEX_H, e);
        Expr tu = field(GLARE_TEX_M00, e) * tx + field(GLARE_TEX_M01, e) * ty + 0.5f * tw;
        Expr tv = field(GLARE_TEX_M10, e) * tx + field(GLARE_TEX_M11, e) * ty + 0.5f * th;
        Expr u0 = floor(tu), v0 = floor(tv), fu = tu - u0, fv = tv - v0;
        Expr iw = cast<int>(tw), ih = cast<int>(th), start = cast<int>(field(GLARE_TEX_START, e));
        auto texel = [&](Expr du, Expr dv) {
            Expr i = cast<int>(u0) + du, j = cast<int>(v0) + dv;
            Expr on = i >= 0 && i < iw && j >= 0 && j < ih;
            Expr at = start + (clamp(j, 0, max(ih - 1, 0)) * iw + clamp(i, 0, max(iw - 1, 0))) * 3 +
                      clamp(c, 0, 2);
            Expr value = texels(clamp(at % GLARE_TEXEL_ROW, 0, GLARE_TEXEL_ROW - 1),
                                clamp(at / GLARE_TEXEL_ROW, 0, texels.dim(1).extent() - 1));
            return select(on, value, 0.0f);
        };
        Expr sampled = (texel(0, 0) * (1.0f - fu) + texel(1, 0) * fu) * (1.0f - fv) +
                       (texel(0, 1) * (1.0f - fu) + texel(1, 1) * fu) * fv;
        Expr image = select(field(GLARE_TEX, e) != 0.0f, sampled, 1.0f);
        // noise_amp's texture and the rays' streaks (glare_style.c): MillSaph's fbm
        auto octaves = [&](Expr u, Expr v, Expr w, Expr persistence, int count) {
            Expr total = 0.0f, amplitude = 1.0f;
            for (int k = 0; k < count; ++k) {
                total += amplitude * ms_noise::perlin3(u * float(1 << k) + 17.3f * k,
                                                       v * float(1 << k) + 5.1f * k, w);
                amplitude *= persistence;
            }
            return total;
        };
        // (each computed only for the elements with it: a sum of one term where asked, else 1)
        RDom tn_on(0, 1, "tn_on"), streak_on(0, 1, "streak_on");
        tn_on.where(field(GLARE_TN_AMP, e) != 0.0f);
        streak_on.where(field(GLARE_STREAK_AMP, e) != 0.0f);
        Expr tn = octaves(dx * field(GLARE_TN_FX, e) + field(GLARE_TN_U, e), dy * field(GLARE_TN_FY, e),
                          field(GLARE_TN_Z, e), field(GLARE_TN_PERSIST, e), 6);
        Expr texture_noise =
            1.0f + sum(max(1.0f + field(GLARE_TN_AMP, e) * tn, 0.0f) - 1.0f + 0.0f * tn_on);
        Expr streak_at = atan2(dy, dx), streak_rho = field(GLARE_STREAK_FREQ, e);
        Expr sn = octaves(cos(streak_at) * streak_rho, sin(streak_at) * streak_rho,
                          field(GLARE_STREAK_SEED, e), field(GLARE_STREAK_PERSIST, e), 3);
        Expr streaks = 1.0f + sum(max(1.0f + field(GLARE_STREAK_AMP, e) * (0.09f + 0.55f * sn),
                                      0.0f) -
                                  1.0f + 0.0f * streak_on);
        Func special("special");
        special(x, y, c, e) =
            ms_glare::element_value(field, dx, dy, c, e, &angular, image, &lookup) * atmo *
            brighter * texture_noise * streaks;
        noisy(x, y, c, e) = special(x, y, c, e) * mask;
        // The sum of the elements, each where it reaches (none of size 0, no shadow's row):
        // those without a shadow, those with one, those with a radius noise (apart: the
        // shadow costs another element's value, the noise its octaves).
        Func sum_of("sum_of");
        enum { PLAIN, SHADOWED, NOISY, NOISY_SHADOWED };
        // Each list's rows, where they reach.
        auto list = [&](Expr first, Expr size, const char *name) {
            RDom r(first, size, name);
            Expr ex = cast<float>(x) + 0.5f - field(GLARE_POS_X, r);
            Expr ey = cast<float>(y) + 0.5f - field(GLARE_POS_Y, r);
            Expr reach = ms_glare::element_reach(field, r) * max(wider(r), 1.0f);
            r.where(ex * ex + ey * ey <= reach * reach);
            return r;
        };
        RDom el = list(0, plain, "el");
        RDom es = list(plain, shadowed_count, "es");
        RDom en = list(plain + shadowed_count, special_count, "en");
        RDom ens = list(plain + shadowed_count + special_count, both, "ens");
        sum_of(x, y, c) = 0.0f;
        sum_of(x, y, c) += element(x, y, c, el);
        sum_of(x, y, c) += shadowed(x, y, c, es);
        sum_of(x, y, c) += special(x, y, c, en);
        sum_of(x, y, c) += noisy(x, y, c, ens);
        flare(x, y, c) = select(c < 3, sum_of(x, y, min(c, 2)) * visible(0, 0, 0), 0.0f);

        texels.dim(0).set_bounds(0, GLARE_TEXEL_ROW);
        rows.dim(0).set_bounds(0, GLARE_FIELDS);
        rows.dim(1).set_bounds(0, GLARE_MAX_ROWS);
        noise_rows.dim(0).set_bounds(0, GLARE_FIELDS);
        noise_rows.dim(1).set_bounds(0, GLARE_MAX_ELEMENTS);
        ms::interleaved(visible);
        ms::interleaved(flare);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        flare.bound(c, 0, 4).reorder(c, x, y);
        sum_of.compute_at(flare, gpu ? xi : x).bound(c, 0, 3).reorder(c, x, y);
        atmosphere.compute_at(flare, gpu ? xi : x);
        // (the noises of every row: none but the noisy ones cost anything, their octaves
        // and taps empty)
        profile.compute_root().bound(c, 0, 3).reorder(k, c, e);
        noises.compute_root().bound(i, 0, ANGLES).bound(e, 0, GLARE_MAX_ELEMENTS).bound(which, 0, 2);
        thresholded.compute_root().bound(i, 0, ANGLES).bound(e, 0, GLARE_MAX_ELEMENTS);
        if (gpu) {
            Var io("io"), ii("ii");
            noises.gpu_tile(i, io, ii, 64);
            Var ko("ko"), ki("ki");
            profile.gpu_tile(k, ko, ki, 128);
            thresholded.gpu_tile(i, io, ii, 64);
        } else {
            noises.parallel(e).vectorize(i, 8);
            profile.parallel(e).vectorize(k, 8);
            thresholded.parallel(e).vectorize(i, 8);
        }
        sum_of.update(0).reorder(c, el);
        sum_of.update(1).reorder(c, es);
        sum_of.update(2).reorder(c, en);
        sum_of.update(3).reorder(c, ens);
        if (gpu)
            flare.unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            flare.unroll(c).parallel(y);
    }
};

class LensFlareGlow : public Generator<LensFlareGlow> {
  public:
    Input<Buffer<float, 3>> flare{"flare"};
    Input<Buffer<float, 3>> occluder{"occluder"};
    Input<int> from{"from"};     // 1: luma, 2: alpha
    Input<int> invert{"invert"};
    Input<Buffer<float, 3>> visible{"visible"}; // ms_lensflare_occlusion's value
    Input<float> hotspot_x{"hotspot_x"}, hotspot_y{"hotspot_y"}; // pixels, y up
    Input<int> edge{"edge"};             // the occluder's blur (Occlusion Softness): whole
    Input<float> edge_frac{"edge_frac"}; // pixels of the 3-box blur's total radius, fraction
    Input<float> reach{"reach"}; // the weight around the hotspot: exp(-(d / reach)^1.7) (0: 1)
    Input<int> blur{"blur"};             // the glow's blur (Glow Width): as `edge`
    Input<float> blur_frac{"blur_frac"};
    Input<float> gain_r{"gain_r"}, gain_g{"gain_g"}, gain_b{"gain_b"}; // Diffraction Glow x Color
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        // The occluder (0..1), blurred; its edges: the gradient's length; weighted by their
        // distance to the hotspot; blurred.
        Func occ("occ"), edges("edges");
        Expr a = select(from == 1, luma(occluder(x, y, 0), occluder(x, y, 1), occluder(x, y, 2)),
                        occluder(x, y, 3));
        a = clamp(a, 0.0f, 1.0f);
        occ(x, y, c) = select(invert != 0, 1.0f - a, a);
        std::vector<ms_blur::Box> boxes;
        Func soft = ms_blur::blur3_subpixel(occ, edge, edge_frac, edge, edge, edge_frac, edge, Expr(0),
                                            occluder.width(), occluder.height(), gpu, "eblur",
                                            boxes);
        Func clamped = BoundaryConditions::repeat_edge(soft, {{0, occluder.width()},
                                                              {0, occluder.height()},
                                                              {0, 1}});
        Expr gx = 0.5f * (clamped(x + 1, y, 0) - clamped(x - 1, y, 0));
        Expr gy = 0.5f * (clamped(x, y + 1, 0) - clamped(x, y - 1, 0));
        Expr ex = cast<float>(x) + 0.5f - hotspot_x, ey = cast<float>(y) + 0.5f - hotspot_y;
        Expr d = sqrt(ex * ex + ey * ey) / max(reach, 1e-6f);
        Expr near = select(reach > 0.0f, exp(-pow(d, 1.70f)), 1.0f);
        edges(x, y, c) = sqrt(gx * gx + gy * gy) * near;
        Func glow = ms_blur::blur3_subpixel(edges, blur, blur_frac, blur, blur, blur_frac, blur,
                                            Expr(0), occluder.width(), occluder.height(), gpu,
                                            "gblur", boxes);
        Expr gain = select(c == 0, gain_r, c == 1, gain_g, gain_b) * visible(0, 0, 0);
        output(x, y, c) = flare(x, y, c) + select(c < 3, glow(x, y, 0) * gain, 0.0f);

        ms::interleaved(flare);
        ms::interleaved(occluder);
        ms::interleaved(visible);
        ms::interleaved(output);
        ms_blur::schedule(boxes, gpu, 1);
        for (Func fn : {occ, edges})
            fn.bound(c, 0, 1);
        tiles(occ.compute_root(), gpu, 1);
        tiles(edges.compute_root(), gpu, 1);
        tiles(output, gpu);
    }
};

class LensFlareTexture : public Generator<LensFlareTexture> {
  public:
    Input<int> kind{"kind"}; // glare_layout.h GLARE_TEXTURE_*
    Input<float> seed{"seed"}, amount{"amount"}, scale{"scale"}, detail{"detail"};
    Input<int> sides{"sides"};
    Output<Buffer<float, 3>> texture{"texture"}; // RGB, row 0 at the top

    void generate() {
        using ms_noise::hash3;
        using ms_noise::perlin3;
        const float PI = ms_glare::PI;
        // (u, v): -1..1 across the texture, v up; r, a: polar
        Expr w = cast<float>(texture.dim(0).extent()), h = cast<float>(texture.dim(1).extent());
        Expr u = (cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
        Expr v = 1.0f - (cast<float>(y) + 0.5f) / h * 2.0f;
        Expr r = sqrt(u * u + v * v), a = atan2(v, u);
        Expr s = floor(seed * 1000.0f); // the seed's lattice offset
        auto random = [&](Expr i, Expr j, int k) { // 0..1 from the cell (i, j)
            return cast<float>(hash3(i + cast<int>(s), j, k) & ms_noise::u32(0xffffff)) /
                   16777216.0f;
        };
        auto spectrum = [&](Expr t, int ch) { // a rainbow: red 0, green 1/3, blue 2/3
            Expr c6 = t * 6.0f;
            Expr value = ch == 0 ? abs(c6 - 3.0f) - 1.0f
                         : ch == 1 ? 2.0f - abs(c6 - 2.0f)
                                   : 2.0f - abs(c6 - 4.0f);
            return clamp(value, 0.0f, 1.0f);
        };
        auto smooth = [&](Expr t) {
            t = clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };

        // DIRT: two layers of specks, one in each cell of a grid (amount: how many cells
        // have one), each a soft disc brighter at its rim; smudges of noise (detail).
        auto specks = [&](float cells, int layer) {
            Expr n = cells * scale;
            Expr gx = (u * 0.5f + 0.5f) * n, gy = (v * 0.5f + 0.5f) * n;
            Expr ci = cast<int>(floor(gx)), cj = cast<int>(floor(gy));
            Expr total = 0.0f;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    Expr i = ci + di, j = cj + dj;
                    Expr there = random(i, j, 10 * layer) < amount;
                    Expr px = cast<float>(i) + random(i, j, 10 * layer + 1);
                    Expr py = cast<float>(j) + random(i, j, 10 * layer + 2);
                    Expr radius = 0.15f + 0.35f * random(i, j, 10 * layer + 3);
                    Expr bright = 0.15f + 0.5f * random(i, j, 10 * layer + 4);
                    Expr d = sqrt((gx - px) * (gx - px) + (gy - py) * (gy - py)) / radius;
                    Expr disc = (1.0f - smooth((d - 0.7f) / 0.3f)) * (0.55f + 0.45f * d * d);
                    total += select(there, bright * disc, 0.0f);
                }
            return total;
        };
        Expr smudge = smooth((ms_noise::fbm(u * 2.0f * scale + 3.1f, v * 2.0f * scale + 7.3f,
                                            s * 0.01f, 5, 0.55f, 5, "smudge") +
                              0.1f) *
                             1.5f) *
                      0.2f * detail;
        Expr dirt = min(specks(7.0f, 1) * 0.8f + specks(17.0f, 2) * 0.5f + smudge, 1.0f);

        // GLINT: a streak along u (amount: its length), thin across (detail: its width),
        // coloured by a rainbow along it, a fainter copy beside it.
        auto streak = [&](Expr shift, Expr strength, int ch) {
            Expr len = 0.3f + 0.7f * clamp(amount, 0.0f, 1.0f);
            Expr width = 0.012f + 0.08f * clamp(detail, 0.0f, 1.0f);
            Expr vv = v - shift;
            Expr along = 1.0f - smooth(abs(u) / len);
            Expr across = exp(-(vv * vv) / (2.0f * width * width * (1.0f + 2.0f * u * u)));
            Expr t = clamp(0.5f + 0.5f * u / len, 0.0f, 1.0f) * 0.85f;
            return strength * along * across * (0.15f + 0.85f * spectrum(t, ch));
        };
        auto glint = [&](int ch) {
            Expr offset = 0.15f + 0.3f * random(0, 0, 20);
            return clamp(streak(0.0f, 1.0f, ch) + streak(offset, 0.35f, ch), 0.0f, 1.0f);
        };

        // RAMP: a rainbow over the radius (sides 0) or across u (sides 1); amount: the share
        // of the rainbow, detail: a fade towards the outer end.
        auto ramp = [&](int ch) {
            Expr t = select(sides == 1, u * 0.5f + 0.5f, r);
            Expr fade = select(sides == 1, 1.0f,
                               (1.0f - detail * smooth((r - 0.6f) / 0.4f)) *
                                   (1.0f - smooth((r - 0.92f) / 0.08f)));
            Expr hue = t * clamp(amount, 0.05f, 1.0f) * 0.85f;
            return spectrum(hue, ch) * fade;
        };

        // IRIS: a polygon of `sides` blades (detail: its corners' roundness), filled with fine
        // radial streaks (amount: their strength) and faint ripples, a brighter rim.
        Expr n_sides = cast<float>(max(sides, 3));
        Expr seg = 2.0f * PI / n_sides;
        Expr phase = a + PI / 2.0f;
        Expr phi = phase - seg * floor(phase / seg) - seg / 2.0f;
        Expr edge = cos(PI / n_sides) / cos(phi);
        edge = edge + (1.0f - edge) * clamp(detail, 0.0f, 1.0f);
        Expr t = r / (0.95f * edge);
        Expr rho = 40.0f * scale;
        Expr rays = 0.5f + 0.5f * clamp(perlin3(cos(a) * rho + 5.0f, sin(a) * rho + 2.0f, s * 0.01f) / 0.3f,
                                        -1.0f, 1.0f);
        Expr ripples = 0.92f + 0.08f * cos(t * 2.0f * PI * 9.0f + s);
        Expr rim = 1.0f + 0.35f * smooth((t - 0.8f) / 0.2f);
        rays = 1.0f + (rays - 1.0f) * smooth(t / 0.35f); // (none at the centre: no moire)
        Expr iris = 0.75f * (1.0f - amount * (1.0f - rays)) * ripples * rim *
                    (1.0f - smooth((t - 0.97f) / 0.03f));

        Func out("out");
        Expr grey = select(kind == GLARE_TEXTURE_DIRT, dirt, iris);
        out(x, y, c) = select(kind == GLARE_TEXTURE_GLINT,
                              select(c == 0, glint(0), c == 1, glint(1), glint(2)),
                              kind == GLARE_TEXTURE_RAMP,
                              select(c == 0, ramp(0), c == 1, ramp(1), ramp(2)), grey);
        texture(x, y, c) = clamp(out(x, y, c), 0.0f, 1.0f);

        texture.dim(0).set_stride(3);
        texture.dim(2).set_stride(1).set_bounds(0, 3);
        texture.bound(c, 0, 3).reorder(c, x, y).unroll(c).parallel(y);
    }
};

class LensFlareFinish : public Generator<LensFlareFinish> {
  public:
    Input<Buffer<float, 3>> flare{"flare"};
    Input<Buffer<float, 3>> source{"source"};
    Input<float> gain_r{"gain_r"}, gain_g{"gain_g"}, gain_b{"gain_b"}; // Brightness x Color
    Input<float> hue{"hue"}, saturation{"saturation"};
    Input<int> blur{"blur"}; // Blur Flare: whole pixels
    Input<float> blur_frac{"blur_frac"};
    Input<float> bg{"bg"};          // Bg Brightness
    Input<int> combine{"combine"};  // 0: Screen, 1: Add, 2: Flare Only
    Input<int> tint{"tint"};        // Tint Bg Whites
    Input<float> affect_alpha{"affect_alpha"};
    // Flicker: the flare's brightness x 1 + amp F, F MillSaph's noise over time (at z) with a
    // dead zone (dead) and a scale (scale) by Flicker Randomness
    Input<float> flicker_amp{"flicker_amp"}, flicker_z{"flicker_z"};
    Input<float> flicker_dead{"flicker_dead"}, flicker_scale{"flicker_scale"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func lit("lit"), hued("hued"), turned("turned");
        // (sd 0.18: the reference's at Randomness 0; the noise's own sd 0.2816)
        Expr n = ms_noise::perlin3(0.37f, 0.71f, flicker_z) * (0.18f / 0.2816f);
        Expr past = max(abs(n) - flicker_dead, 0.0f) * flicker_scale;
        Expr flicker = max(1.0f + flicker_amp * select(n < 0.0f, -past, past), 0.0f);
        Expr gain = select(c == 0, gain_r, c == 1, gain_g, gain_b) * flicker;
        lit(x, y, c) = select(c < 3, flare(x, y, min(c, 2)) * gain, 0.0f);
        hued(x, y, c) = ms::hue_shifted(lit(x, y, 0), lit(x, y, 1), lit(x, y, 2), c, hue,
                                        lit(x, y, c));
        Expr l = luma(hued(x, y, 0), hued(x, y, 1), hued(x, y, 2));
        turned(x, y, c) = select(c < 3, l + (hued(x, y, c) - l) * saturation, 0.0f);
        std::vector<ms_blur::Box> boxes;
        Func blurred = ms_blur::blur3_subpixel(turned, blur, blur_frac, blur, blur, blur_frac, blur,
                                               Expr(0), source.width(), source.height(), gpu,
                                               "fblur", boxes);
        Func f("f");
        f(x, y, c) = blurred(x, y, c);
        // Screen: s + f (1 - s); Add: s + f; Flare Only: f, alpha 0. Tint Bg Whites: the
        // background tinted by the flare's colour, s x (f - its luma).
        Expr s = source(x, y, c) * bg, a = source(x, y, 3);
        Expr fc = f(x, y, c);
        Expr fl = luma(f(x, y, 0), f(x, y, 1), f(x, y, 2));
        Expr screen = s + fc * max(1.0f - s, 0.0f);
        Expr rgb = select(combine == 0, screen, combine == 1, s + fc, fc);
        rgb = rgb + select(tint != 0 && combine != 2, s * (fc - fl), 0.0f);
        Expr flare_alpha = max(f(x, y, 0), max(f(x, y, 1), f(x, y, 2))) * affect_alpha;
        Expr base = select(combine == 2, 0.0f, a);
        output(x, y, c) = select(c == 3, base + (1.0f - base) * flare_alpha, rgb);

        ms::interleaved(flare);
        ms::interleaved(source);
        ms::interleaved(output);
        ms_blur::schedule(boxes, gpu, 4);
        for (Func fn : {turned, f})
            tiles(fn.compute_root(), gpu);
        tiles(output, gpu);
    }
};

HALIDE_REGISTER_GENERATOR(LensFlareOcclusion, ms_lensflare_occlusion)
HALIDE_REGISTER_GENERATOR(LensFlareDraw, ms_lensflare_draw)
HALIDE_REGISTER_GENERATOR(LensFlareGlow, ms_lensflare_glow)
HALIDE_REGISTER_GENERATOR(LensFlareTexture, ms_lensflare_texture)
HALIDE_REGISTER_GENERATOR(LensFlareFinish, ms_lensflare_finish)
