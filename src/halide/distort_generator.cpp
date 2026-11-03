// MS_Distort in Halide, pipelines that effects/ms_distort.c runs one after the other (on the
// GPU the images stay there between them):
//   ms_mask            the mask value (only with a Mask; mask_generator.cpp)
//   ms_distort_shift   lens, shift per pixel, level of detail (Filter)
//   ms_distort_levels  the reduced copies of the Source the Filter reads, in one image
//   ms_distort_warp    the warped Source, mixed with the Source by the mask
// See effects/ms_distort.c for what the effect does.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"
#include "mask.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

constexpr int MAX_LEVELS = 16; // reduced copies per axis, at most (2^15 pixels)

// Level of detail for a footprint of d pixels: log2 with a linear step between powers of
// two (level n at d = 2^n), 0 below 1. Exact: from the float's exponent and mantissa.
Expr fast_log2(Expr d) {
    Expr bits = reinterpret<int>(d);
    Expr exponent = (bits >> 23) - 127;
    Expr mantissa = reinterpret<float>((bits & 0x7fffff) | 0x3f800000); // in [1, 2)
    return select(d <= 1.0f, 0.0f, cast<float>(exponent) + mantissa - 1.0f);
}

// How fast the shift changes along one axis: its two components combined as max + min / 2.
Expr footprint(Expr a, Expr b) {
    a = abs(a);
    b = abs(b);
    return max(a, b) + 0.5f * min(a, b);
}

// 2^-n, exactly: GPU code is compiled with fast math, where x / 2^n can be a bit off (and a
// bit off moves a shift rounded to 1/256 pixel). Divisions by powers of two are written as
// products with this.
Expr inverse_power_of_two(Expr n) { return reinterpret<float>((127 - n) << 23); }

// Where level n starts along an axis of `length` pixels, in the image of all the levels.
Expr level_offset(Expr length, Expr n) {
    Expr o = 0;
    for (int k = 0; k < MAX_LEVELS - 1; ++k)
        o += select(k < n, length >> k, 0);
    return o;
}

} // namespace

// The lens (luma of the Lens, times the mask with Mask Lens, blurred by Blur Lens) and from
// it the shift of each pixel, rounded to 1/256 pixel; with the level of detail per axis (how
// fast the shift changes) and its largest value over the image.
class DistortShift : public Generator<DistortShift> {
  public:
    Input<Buffer<float, 3>> lens{"lens"}; // the Source when no Lens is connected
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // DistortMask; none: one value (stride 0)
    Input<int> mask_lens{"mask_lens"};                // and a mask connected
    Input<int> blur_lens{"blur_lens"};                // total radius in pixels
    Input<float> amount{"amount"}; // pixels of shift per unit of lens slope (gain included)
    Input<float> rel_x{"rel_x"}, rel_y{"rel_y"}, cos_r{"cos_r"}, sin_r{"sin_r"};
    Output<Buffer<float, 3>> shift{"shift"};             // x, y, level of detail x, y
    Output<Buffer<float, 1>> largest_lod{"largest_lod"}; // [x, y]

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr width = lens.width(), height = lens.height();
        std::vector<ms_blur::Box> boxes;

        // Lens, blurred.
        Func l("l");
        l(x, y, c) = ms_mask::luma(lens, x, y) * select(mask_lens != 0, mask_value(x, y), 1.0f);
        Func lb = ms_blur::blur3(l, blur_lens, blur_lens, width, height, gpu, "lensb_", boxes,
                                 ms_blur::Boxes::sliding);

        // Shift: slope of the lens (central difference, one-sided at the borders) times
        // amount, per axis, turned by the warp direction, rounded to 1/256 pixel.
        Func s("s");
        Expr xl = max(x - 1, 0), xr = min(x + 1, width - 1);
        Expr yb = max(y - 1, 0), yt = min(y + 1, height - 1);
        // The neighbours are 2 pixels apart, or 1 at a border: a product, exact (see
        // inverse_power_of_two).
        auto per_pixel = [](Expr gap) { return select(gap == 2, 0.5f, gap == 1, 1.0f, 0.0f); };
        Expr gx = (lb(xr, y, 0) - lb(xl, y, 0)) * per_pixel(xr - xl);
        Expr gy = (lb(x, yt, 0) - lb(x, yb, 0)) * per_pixel(yt - yb);
        Expr vx = amount * rel_x * gx, vy = amount * rel_y * gy;
        auto round = [](Expr e) { return floor(e * 256.0f + 0.5f) * (1.0f / 256.0f); };
        s(x, y) = {round(cos_r * vx - sin_r * vy), round(sin_r * vx + cos_r * vy)};

        // Level of detail per axis, from the derivatives of the shift (central differences,
        // one-sided at the borders).
        Expr hx = select(xr - xl == 2, 0.5f, 1.0f), hy = select(yt - yb == 2, 0.5f, 1.0f);
        Func lod("lod");
        lod(x, y) = {fast_log2(footprint((s(xr, y)[0] - s(xl, y)[0]) * hx,
                                         (s(xr, y)[1] - s(xl, y)[1]) * hx)),
                     fast_log2(footprint((s(x, yt)[0] - s(x, yb)[0]) * hy,
                                         (s(x, yt)[1] - s(x, yb)[1]) * hy))};
        shift(x, y, c) = mux(c, {s(x, y)[0], s(x, y)[1], lod(x, y)[0], lod(x, y)[1]});

        // Largest level of detail: per row, then over the rows.
        Func row("row");
        RDom rx(0, width, "rx"), ry(0, height, "ry");
        row(y) = {maximum(lod(rx, y)[0]), maximum(lod(rx, y)[1])};
        largest_lod(c) = mux(c, {maximum(row(ry)[0]), maximum(row(ry)[1])});

        ms::interleaved(lens);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        ms::interleaved(shift);
        largest_lod.dim(0).set_bounds(0, 2);

        // Schedule.
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 1);
        if (gpu) {
            s.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            shift.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            row.compute_root().gpu_tile(y, yo, yi, 64);
            largest_lod.bound(c, 0, 2).gpu_single_thread();
        } else {
            s.compute_root().vectorize(x, 8).parallel(y);
            shift.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
            row.compute_root().parallel(y, 16);
            largest_lod.bound(c, 0, 2);
        }
    }
};

// The reduced copies of the Source the Filter reads, in one image: level (nx, ny) is the
// mean of blocks of 2^nx x 2^ny pixels (an odd last row or column dropped at each halving),
// (W >> nx) x (H >> ny) pixels, at (level_offset(W, nx), level_offset(H, ny)). Levels up to
// levels_x - 1 and levels_y - 1; the output must be exactly as large as they need.
class DistortLevels : public Generator<DistortLevels> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<int> levels_x{"levels_x"}, levels_y{"levels_y"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr width = source.width(), height = source.height();
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        const TailStrategy guard = TailStrategy::GuardWithIf;

        // Level 0 is the Source; along x, level n halves level n - 1, side by side (rows of
        // the Source); then along y, level n halves level n - 1 of every column of that. All
        // in the output (no image of Halide's own on the GPU, where it would be allocated at
        // every call). A level not needed is one pixel of level 0 written back unchanged: the
        // accesses stay inside the image (Halide checks them whatever the levels).
        output(x, y, c) = select(x < width && y < height,
                                 source(clamp(x, 0, width - 1), clamp(y, 0, height - 1), c), 0.0f);
        std::vector<RDom> rx, ry;
        for (int n = 1; n < MAX_LEVELS; ++n) {
            Expr used = n < levels_x;
            RDom r(0, select(used, max(width >> n, 1), 1), 0, select(used, height, 1),
                   "rx" + std::to_string(n));
            Expr to = select(used, level_offset(width, n), 0) + r.x;
            Expr from = select(used, level_offset(width, n - 1) + 2 * r.x, 0);
            output(to, r.y, c) = select(used, 0.5f * (output(from, r.y, c) + output(from + 1, r.y, c)),
                                        output(to, r.y, c));
            rx.push_back(r);
        }
        Expr all_width = output.width();
        for (int n = 1; n < MAX_LEVELS; ++n) {
            Expr used = n < levels_y;
            RDom r(0, select(used, all_width, 1), 0, select(used, max(height >> n, 1), 1),
                   "ry" + std::to_string(n));
            Expr to = select(used, level_offset(height, n), 0) + r.y;
            Expr from = select(used, level_offset(height, n - 1) + 2 * r.y, 0);
            output(r.x, to, c) = select(used, 0.5f * (output(r.x, from, c) + output(r.x, from + 1, c)),
                                        output(r.x, to, c));
            ry.push_back(r);
        }

        ms::interleaved(source);
        ms::interleaved(output);

        // Schedule: each level is a pass over its pixels (they only read the level before:
        // no race between them).
        output.bound(c, 0, 4).reorder(c, x, y).vectorize(c);
        if (gpu)
            output.gpu_tile(x, y, xo, yo, xi, yi, 16, 16, guard);
        else
            output.parallel(y, 16);
        RVar rxo("rxo"), ryo("ryo"), rxi("rxi"), ryi("ryi");
        for (int k = 0; k < 2 * (MAX_LEVELS - 1); ++k) {
            RDom &r = k < MAX_LEVELS - 1 ? rx[k] : ry[k - (MAX_LEVELS - 1)];
            Stage st = output.update(k);
            st.allow_race_conditions().reorder(c, r.x, r.y).vectorize(c);
            if (gpu)
                st.gpu_tile(r.x, r.y, rxo, ryo, rxi, ryi, 16, 16, guard);
            else
                st.parallel(r.y, 16);
        }
    }
};

// Output pixel p = Source at p + shift, bilinear, with the Wrap modes inside the crop. With
// Filter, it reads the reduced copies (DistortLevels) where the warp shrinks the image,
// blending two levels. Then the mask: mixed after the warp (Post-effect) or moved with it
// (Pre-effect).
class DistortWarp : public Generator<DistortWarp> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> shift{"shift"}; // DistortShift
    Input<Buffer<float, 2>> mask_value{"mask_value"};
    Input<Buffer<float, 3>> levels{"levels"}; // DistortLevels; without Filter, the Source
    Input<int> filter{"filter"}, levels_x{"levels_x"}, levels_y{"levels_y"};
    Input<int> wrap_x{"wrap_x"}, wrap_y{"wrap_y"}; // 0 No, 1 Tile, 2 Reflect
    Input<float> crop_l{"crop_l"}, crop_r{"crop_r"}, crop_b{"crop_b"}, crop_t{"crop_t"};
    Input<int> has_mask{"has_mask"}, pre_effect{"pre_effect"};
    Output<Buffer<float, 3>> output{"output"};

    // Coordinate s brought inside [lo, hi) with the wrap mode: Tile repeats the pixels,
    // Reflect mirrors them about lo and hi. `inside`: false if nothing can be read there
    // (Wrap No, more than a pixel outside, or nothing left by the crop).
    struct Wrapped {
        Expr s, inside;
    };
    static Wrapped wrap(Expr s, Expr lo, Expr hi, Expr mode) {
        Expr n = hi - lo;
        auto mod = [](Expr a, Expr b) { return a - b * floor(a / b); };
        Expr tile = lo + mod(s - lo, n);
        Expr reflect = lo + mod(s - lo, 2.0f * n);
        reflect = select(reflect > hi, 2.0f * hi - reflect, reflect);
        reflect = min(reflect, hi - 1.0f);
        Expr no = s > lo - 1.0f && s < hi;
        return {select(mode == 0, s, mode == 1, tile, reflect), n > 0 && (mode != 0 || no)};
    }

    // Pixel read for index i of a level `length` pixels long whose pixels cover 2^n Source
    // pixels (the crop [lo_s, hi_s) in Source pixels); -1 for transparent (Wrap No).
    static Expr tap(Expr i, Expr length, Expr n, Expr lo_s, Expr hi_s, Expr mode) {
        Expr lo = cast<int>(ceil(lo_s * inverse_power_of_two(n)));
        Expr hi = min(cast<int>(floor(hi_s * inverse_power_of_two(n))), length);
        hi = max(hi, lo + 1);
        Expr outside = select(mode == 0, -1, mode == 1, select(i < lo, i + (hi - lo), i - (hi - lo)),
                              select(i < lo, lo, hi - 1));
        return select(i >= lo && i < hi, i, outside);
    }

    void generate() {
        Expr width = source.width(), height = source.height();
        Expr lo_x = crop_l, hi_x = cast<float>(width) - crop_r;
        Expr lo_y = crop_b, hi_y = cast<float>(height) - crop_t;
        Expr levels_width = levels.width() - 1, levels_height = levels.height() - 1;

        // Bilinear sample of level (nx, ny) at Source position (sx, sy), channel c.
        auto level = [&](Expr nx, Expr ny, Expr sx, Expr sy) {
            Expr w = width >> nx, h = height >> ny;
            Expr ox = level_offset(width, nx), oy = level_offset(height, ny);
            Expr qx = (sx + 0.5f) * inverse_power_of_two(nx) - 0.5f;
            Expr qy = (sy + 0.5f) * inverse_power_of_two(ny) - 0.5f;
            Expr fx = floor(qx), fy = floor(qy);
            Expr ix = cast<int>(fx), iy = cast<int>(fy);
            Expr sum = 0.0f;
            for (int j = 0; j < 2; ++j) {
                Expr wy = j == 0 ? 1.0f - (qy - fy) : qy - fy;
                Expr ty = tap(iy + j, h, ny, lo_y, hi_y, wrap_y);
                for (int i = 0; i < 2; ++i) {
                    Expr wx = i == 0 ? 1.0f - (qx - fx) : qx - fx;
                    Expr tx = tap(ix + i, w, nx, lo_x, hi_x, wrap_x);
                    Expr value = levels(clamp(ox + tx, 0, levels_width),
                                        clamp(oy + ty, 0, levels_height), c);
                    sum += select(tx >= 0 && ty >= 0, wx * wy * value, 0.0f);
                }
            }
            return sum;
        };

        Expr sh_x = shift(x, y, 0), sh_y = shift(x, y, 1);
        Wrapped px = wrap(cast<float>(x) + sh_x, lo_x, hi_x, wrap_x);
        Wrapped py = wrap(cast<float>(y) + sh_y, lo_y, hi_y, wrap_y);
        // Levels below and above, blended by the fraction of the larger level.
        Expr lx = select(filter != 0, min(shift(x, y, 2), cast<float>(levels_x - 1)), 0.0f);
        Expr ly = select(filter != 0, min(shift(x, y, 3), cast<float>(levels_y - 1)), 0.0f);
        Expr nx0 = cast<int>(floor(lx)), ny0 = cast<int>(floor(ly));
        Expr nx1 = cast<int>(ceil(lx)), ny1 = cast<int>(ceil(ly));
        Expr f = select(lx >= ly, lx - cast<float>(nx0), ly - cast<float>(ny0));
        Expr warped = (1.0f - f) * level(nx0, ny0, px.s, py.s) +
                      select(f > 0, f * level(nx1, ny1, px.s, py.s), 0.0f);
        warped = select(px.inside && py.inside, warped, 0.0f);

        // The mask: at this pixel (Post-effect), or moved with the image (Pre-effect).
        Wrapped mx = wrap(cast<float>(x) + sh_x, 0.0f, cast<float>(width), select(wrap_x == 1, 1, 2));
        Wrapped my = wrap(cast<float>(y) + sh_y, 0.0f, cast<float>(height), select(wrap_y == 1, 1, 2));
        Expr ax = cast<int>(floor(mx.s)), ay = cast<int>(floor(my.s));
        Expr cx = select(ax + 1 < width, ax + 1, ax), cy = select(ay + 1 < height, ay + 1, ay);
        Expr fx = mx.s - cast<float>(ax), fy = my.s - cast<float>(ay);
        auto mv = [&](Expr px_, Expr py_) {
            return mask_value(clamp(px_, 0, width - 1), clamp(py_, 0, height - 1));
        };
        Expr moved = lerp(lerp(mv(ax, ay), mv(cx, ay), fx), lerp(mv(ax, cy), mv(cx, cy), fx), fy);
        moved = select(mx.inside && my.inside, moved, 0.0f);
        Expr m = select(has_mask == 0, 1.0f, pre_effect != 0, moved, mask_value(x, y));

        Expr src = source(x, y, c);
        output(x, y, c) = src + (warped - src) * m;

        ms::interleaved(source);
        ms::interleaved(shift);
        ms::interleaved(levels);
        ms::interleaved(output);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)

        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        output.bound(c, 0, 4).reorder(c, x, y).vectorize(c);
        if (get_target().has_gpu_feature())
            output.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            output.parallel(y, 4);
    }
};

HALIDE_REGISTER_GENERATOR(DistortShift, ms_distort_shift)
HALIDE_REGISTER_GENERATOR(DistortLevels, ms_distort_levels)
HALIDE_REGISTER_GENERATOR(DistortWarp, ms_distort_warp)
