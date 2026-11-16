// MS_JpegDamage in Halide: the image through a JPEG round trip, the way the IJG library
// (libjpeg) does it in integers, called by effects/ms_jpegdamage.c:
//   - Res Factor: the image at fx x fy times less resolution, each cell of fx x fy pixels its
//     bottom-left pixel (x right, y up, cells from the bottom-left corner); the result back
//     cell by cell
//   - samples floor(v x 256), 0 to 255; the image extended to whole 16 x 16 macroblocks by its
//     edges (from the bottom-left corner)
//   - RGB to YCbCr (fixed point, 16 bits), the chroma halved both ways (2 x 2 means, the
//     rounding bias alternating 1, 2 by column)
//   - 8 x 8 blocks, each read top row first: the accurate integer DCT, quantised (the
//     rounded quotient by the table's step), dequantised by the decoder's steps (the table's x
//     the frequency gains), the accurate integer inverse DCT and its range limit (values past
//     the range wrap as the library's table makes them)
//   - the chroma doubled back (the library's "fancy" triangle filter, rows only within each
//     band of 32 image rows from the bottom: the image is compressed band by band)
//   - YCbCr to RGB (fixed point), samples / 256
//   - the errors (MillSaph's own random, the reference's statistics): added to the dequantised
//     coefficients of damaged blocks. The damaged units (16 columns x 32 rows: a band's
//     macroblocks, both rows) along the bands from the bottom: a smoothed noise along them,
//     cells of Error Coherence x the width (at least one unit), below a threshold (a share
//     Err Block Density^1.6 of the units, as the reference's). In them each coefficient hit
//     with the chance min(0.06 rate^0.75 (the mean: x 3), 0.8), by 10 x 50^U x amp x
//     (1 - 0.05 (u + v))
//     either way (the reference's errors: heavy tailed, lower frequencies stronger; mean
//     damage per damaged macroblock within ~15 % over Rate 0.25-5 and Amp 0.5-5)
//   - then Saturation (around the luma 5/16 R + 8/16 G + 3/16 B), Scale Lights and Offset
//     Darks (darks + (lights - darks) v), mixed over the Source by the mask value
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "noise.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c"), u("u"), v("v"), bx("bx"), by("by"), k("k");

// The errors' chance per coefficient of a damaged block: CHANCE x Error Rate^0.75.
const float CHANCE = 0.06f;

// The library's fixed-point constants (CONST_BITS 13, PASS1_BITS 2).
const int CONST_BITS = 13, PASS1_BITS = 2;
const int FIX_0_298631336 = 2446, FIX_0_390180644 = 3196, FIX_0_541196100 = 4433,
          FIX_0_765366865 = 6270, FIX_0_899976223 = 7373, FIX_1_175875602 = 9633,
          FIX_1_501321110 = 12299, FIX_1_847759065 = 15137, FIX_1_961570560 = 16069,
          FIX_2_053119869 = 16819, FIX_2_562915447 = 20995, FIX_3_072711026 = 25172;

Expr descale(Expr value, int n) { return (value + (1 << (n - 1))) >> n; }

// The odd part shared by the forward and inverse DCTs: from four values, the four
// rotated ones (tmp4..7 of the forward DCT, tmp0..3 of the inverse).
std::vector<Expr> odd_part(Expr t0, Expr t1, Expr t2, Expr t3) {
    Expr z1 = t0 + t3, z2 = t1 + t2, z3 = t0 + t2, z4 = t1 + t3;
    Expr z5 = (z3 + z4) * FIX_1_175875602;
    Expr a = t0 * FIX_0_298631336, b = t1 * FIX_2_053119869, cc = t2 * FIX_3_072711026,
         d = t3 * FIX_1_501321110;
    z1 = z1 * -FIX_0_899976223;
    z2 = z2 * -FIX_2_562915447;
    z3 = z3 * -FIX_1_961570560 + z5;
    z4 = z4 * -FIX_0_390180644 + z5;
    return {a + z1 + z3, b + z2 + z4, cc + z2 + z3, d + z1 + z4};
}

// The forward DCT of 8 values (one pass): 8 outputs; `last`: the second pass's descaling.
std::vector<Expr> fdct(const std::vector<Expr> &d, bool last) {
    Expr t0 = d[0] + d[7], t7 = d[0] - d[7], t1 = d[1] + d[6], t6 = d[1] - d[6];
    Expr t2 = d[2] + d[5], t5 = d[2] - d[5], t3 = d[3] + d[4], t4 = d[3] - d[4];
    Expr t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    const int s = last ? CONST_BITS + PASS1_BITS : CONST_BITS - PASS1_BITS;
    std::vector<Expr> o(8);
    o[0] = last ? descale(t10 + t11, PASS1_BITS) : (t10 + t11) << PASS1_BITS;
    o[4] = last ? descale(t10 - t11, PASS1_BITS) : (t10 - t11) << PASS1_BITS;
    Expr z1 = (t12 + t13) * FIX_0_541196100;
    o[2] = descale(z1 + t13 * FIX_0_765366865, s);
    o[6] = descale(z1 + t12 * -FIX_1_847759065, s);
    std::vector<Expr> r = odd_part(t4, t5, t6, t7);
    o[7] = descale(r[0], s);
    o[5] = descale(r[1], s);
    o[3] = descale(r[2], s);
    o[1] = descale(r[3], s);
    return o;
}

// The inverse DCT of 8 values (one pass): 8 outputs, descaled by `s` bits.
std::vector<Expr> idct(const std::vector<Expr> &in, int s) {
    Expr z1 = (in[2] + in[6]) * FIX_0_541196100;
    Expr t2 = z1 + in[6] * -FIX_1_847759065, t3 = z1 + in[2] * FIX_0_765366865;
    Expr t0 = (in[0] + in[4]) << CONST_BITS, t1 = (in[0] - in[4]) << CONST_BITS;
    Expr t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    std::vector<Expr> r = odd_part(in[7], in[5], in[3], in[1]);
    return {descale(t10 + r[3], s), descale(t11 + r[2], s), descale(t12 + r[1], s),
            descale(t13 + r[0], s), descale(t13 - r[0], s), descale(t12 - r[1], s),
            descale(t11 - r[2], s), descale(t10 - r[3], s)};
}

// The library's range limit after the inverse DCT: v (centred on 0) to a sample, 0 to 255,
// through its table (v & 1023: past the range, values wrap).
Expr range_limit(Expr value) {
    Expr i = value & 1023;
    return select(i < 128, i + 128, i < 512, 255, i < 896, 0, i - 896);
}

Expr mux8(Expr index, const Tuple &t) {
    return mux(index, {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]});
}

// One plane through the 8 x 8 blocks: its samples `plane` (0..255, x right, y up), the
// quantisation steps and the decoder's (`table`, column `which`): its decoded samples.
struct Blocks {
    Func pass1, coef, quant, deq, pass2, inv1, inv2, out;
    Func error; // (the coefficients' errors, as given)
};

Blocks blocks(const std::string &name, Func plane, Func steps, Func decoder, Func error) {
    Blocks b{Func(name + "_pass1"), Func(name + "_coef"), Func(name + "_quant"),
             Func(name + "_deq"),   Func(name + "_pass2"), Func(name + "_inv1"),
             Func(name + "_inv2"),  Func(name + "_out"),  error};
    // (the block's row r, top row first, is image row 8 by + 7 - r)
    auto sample = [&](Expr col, Expr row, Expr bxx, Expr byy) {
        return plane(bxx * 8 + col, byy * 8 + 7 - row) - 128;
    };
    // the DCT: rows (k: the block's row), then columns (u: the column)
    std::vector<Expr> d(8);
    for (int i = 0; i < 8; ++i)
        d[i] = sample(i, k, bx, by);
    b.pass1(k, bx, by) = Tuple(fdct(d, false));
    std::vector<Expr> col(8);
    for (int i = 0; i < 8; ++i)
        col[i] = mux8(u, b.pass1(i, bx, by));
    b.pass2(u, bx, by) = Tuple(fdct(col, true));
    b.coef(u, v, bx, by) = mux8(v, b.pass2(u, bx, by));
    // quantised: the quotient by 8 x the step, rounded half away from 0
    Expr f = b.coef(u, v, bx, by), q8 = steps(u, v) * 8;
    Expr mag = (cast<int>(abs(f)) + (q8 >> 1)) / q8;
    b.quant(u, v, bx, by) = select(f < 0, -mag, mag);
    b.deq(u, v, bx, by) =
        clamp(b.quant(u, v, bx, by) * decoder(u, v) + error(u, v, bx, by), -(1 << 18), 1 << 18);
    // the inverse DCT: columns (u), then rows (k)
    std::vector<Expr> in(8);
    for (int i = 0; i < 8; ++i)
        in[i] = b.deq(u, i, bx, by);
    b.inv1(u, bx, by) = Tuple(idct(in, CONST_BITS - PASS1_BITS));
    std::vector<Expr> row(8);
    for (int i = 0; i < 8; ++i)
        row[i] = mux8(k, b.inv1(i, bx, by));
    std::vector<Expr> outs = idct(row, CONST_BITS + PASS1_BITS + 3);
    for (Expr &o : outs)
        o = range_limit(o);
    b.inv2(k, bx, by) = Tuple(outs);
    // back to the image: row y is the block's row 7 - y % 8
    b.out(x, y) = mux8(x % 8, b.inv2(7 - y % 8, x / 8, y / 8));
    return b;
}

} // namespace

class JpegDamage : public Generator<JpegDamage> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // ms_mask; none: 1 (strides 0)
    // The quantisation steps and the decoder's (u, v, table: 0 luma, 1 chroma; whole numbers)
    Input<Buffer<float, 3>> steps{"steps"};
    Input<Buffer<float, 3>> decoder{"decoder"};
    Input<int> fx{"fx"}, fy{"fy"}; // Res Factor's cells, in pixels
    // the errors: Error Rate, Error Amp, Err Block Density, Error Coherence; the seed and the
    // frames' epoch (MillSaph's random), Flip Noise Vertically
    // (density: the noise's threshold for Err Block Density, effects/ms_jpegdamage.c)
    Input<float> rate{"rate"}, amp{"amp"}, density{"density"}, coherence{"coherence"};
    Input<int> seed{"seed"}, epoch{"epoch"}, flip{"flip"};
    Input<float> saturation{"saturation"}, lights{"lights"}, darks{"darks"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        // the image at Res Factor's resolution: ws x hs cells
        Expr w = source.dim(0).extent(), h = source.dim(1).extent();
        Expr ws = (w + fx - 1) / fx, hs = (h + fy - 1) / fy;
        Expr w16 = (ws + 15) / 16 * 16;

        // 8-bit samples, the image extended by its edges
        Func s8("s8");
        Expr in = source(min(clamp(x, 0, ws - 1) * fx, w - 1), min(clamp(y, 0, hs - 1) * fy, h - 1), c);
        s8(x, y, c) = clamp(cast<int>(floor(in * 256.0f)), 0, 255);
        Func ycc("ycc");
        Expr R = s8(x, y, 0), G = s8(x, y, 1), B = s8(x, y, 2);
        ycc(x, y) = Tuple((19595 * R + 38470 * G + 7471 * B + 32768) >> 16,
                          (-11059 * R - 21709 * G + 32768 * B + (128 << 16) + 32767) >> 16,
                          (32768 * R - 27439 * G - 5329 * B + (128 << 16) + 32767) >> 16);
        Func luma("luma");
        luma(x, y) = ycc(x, y)[0];
        // the chroma halved: (the 2 x 2 sum + bias) >> 2, bias 1 at even columns, 2 at odd
        Func half_cb("half_cb"), half_cr("half_cr");
        for (int i : {1, 2}) {
            Func &f = i == 1 ? half_cb : half_cr;
            Expr sum = ycc(2 * x, 2 * y)[i] + ycc(2 * x + 1, 2 * y)[i] + ycc(2 * x, 2 * y + 1)[i] +
                       ycc(2 * x + 1, 2 * y + 1)[i];
            f(x, y) = (sum + 1 + (x & 1)) >> 2;
        }

        Func step_l("step_l"), step_c("step_c"), dec_l("dec_l"), dec_c("dec_c");
        step_l(u, v) = cast<int>(steps(u, v, 0));
        step_c(u, v) = cast<int>(steps(u, v, 1));
        dec_l(u, v) = cast<int>(decoder(u, v, 0));
        dec_c(u, v) = cast<int>(decoder(u, v, 1));
        // The errors of plane p's block (bx, by): its unit (macroblock column, band) damaged or
        // not, then each coefficient.
        Expr units = w16 / 16, bands = ((hs + 15) / 16 * 16 + 31) / 32;
        auto uniform = [](Expr h) { return cast<float>(h >> 8) * (1.0f / 16777216.0f); };
        auto errors = [&](int p, int per_unit) {
            Func e("error" + std::to_string(p));
            Expr col = bx / per_unit, band = by / (2 * per_unit);
            band = select(flip != 0, bands - 1 - band, band);
            Expr cell = max(coherence * cast<float>(units), 1.0f);
            Expr s = cast<float>(band * units + col) / cell;
            Expr i = cast<int>(floor(s)), t = s - floor(s);
            Expr h0 = uniform(ms_noise::hash3(i, seed, epoch)), h1 = uniform(ms_noise::hash3(i + 1, seed, epoch));
            Expr k = t * t * (3.0f - 2.0f * t);
            Expr damaged = h0 + (h1 - h0) * k < density;
            Expr rows = (hs + 15) / 16 * 16 / (per_unit == 2 ? 8 : 16); // (the plane's block rows)
            Expr row = select(flip != 0, rows - 1 - by, by);
            Expr hb = ms_noise::hash3(bx + 65536 * p, row, seed * 31 + epoch);
            Expr hit = uniform(ms_noise::hash3(hb, u + 8 * v, 1));
            Expr size = uniform(ms_noise::hash3(hb, u + 8 * v, 2));
            Expr sign = (ms_noise::hash3(hb, u + 8 * v, 3) & ms_noise::u32(1)) == ms_noise::u32(1);
            // (upside down: the odd vertical frequencies change sign)
            sign = select(flip != 0 && (v & 1) == 1, !sign, sign);
            // (the mean three times as often: flat coloured blocks, as the reference's)
            Expr chance = min(CHANCE * pow(rate, 0.75f) * select(u == 0 && v == 0, 3.0f, 1.0f), 0.8f);
            Expr magnitude = 10.0f * exp(size * 3.912f) * amp * (1.0f - 0.05f * cast<float>(u + v));
            Expr value = cast<int>(round(select(sign, magnitude, -magnitude)));
            e(u, v, bx, by) = select(damaged && hit < chance && rate > 0.0f, value, 0);
            return e;
        };
        Blocks bl = blocks("y", luma, step_l, dec_l, errors(0, 2));
        Blocks bb = blocks("cb", half_cb, step_c, dec_c, errors(1, 1));
        Blocks br = blocks("cr", half_cr, step_c, dec_c, errors(2, 1));

        // the chroma doubled back: within the band of 32 rows (16 chroma rows), the nearer row
        // x 3 + the farther one, then across, 3 x this column + the neighbour's, + 8 (even
        // columns, the left neighbour) or + 7 (odd, the right), / 16; edges repeated
        Expr cw = w16 / 2;
        auto upsample = [&](Func half, const std::string &name) {
            Func colsum(name + "_colsum"), full(name + "_full");
            Expr j = y / 2, band0 = (y / 32) * 16;
            Expr far = clamp(select((y & 1) == 1, j + 1, j - 1), band0, band0 + 15);
            colsum(x, y) = half(clamp(x, 0, cw - 1), j) * 3 + half(clamp(x, 0, cw - 1), far);
            Expr i = x / 2;
            Expr other = select((x & 1) == 0, clamp(i - 1, 0, cw - 1), clamp(i + 1, 0, cw - 1));
            full(x, y) = (colsum(i, y) * 3 + colsum(other, y) + select((x & 1) == 0, 8, 7)) >> 4;
            return std::make_pair(colsum, full);
        };
        auto cb_up = upsample(bb.out, "cb");
        auto cr_up = upsample(br.out, "cr");

        // YCbCr to RGB (the library's tables), then to floats
        Func rgb("rgb");
        Expr Y = bl.out(x, y), cb = cb_up.second(x, y) - 128, cr = cr_up.second(x, y) - 128;
        Expr r8 = clamp(Y + ((91881 * cr + 32768) >> 16), 0, 255);
        Expr g8 = clamp(Y + ((-22554 * cb + 32768 - 46802 * cr) >> 16), 0, 255);
        Expr b8 = clamp(Y + ((116130 * cb + 32768) >> 16), 0, 255);
        rgb(x, y) = Tuple(cast<float>(r8) * (1.0f / 256), cast<float>(g8) * (1.0f / 256),
                          cast<float>(b8) * (1.0f / 256));

        // Saturation, Scale Lights / Offset Darks, the mask
        Expr cx = x / fx, cy = y / fy; // (the pixel's cell)
        Expr fr = rgb(cx, cy)[0], fg = rgb(cx, cy)[1], fb = rgb(cx, cy)[2];
        Expr l = 0.3125f * fr + 0.5f * fg + 0.1875f * fb;
        Expr chan = select(c == 0, fr, c == 1, fg, fb);
        Expr adjusted = darks + (lights - darks) * (l + saturation * (chan - l));
        Expr s = source(x, y, c);
        Expr m = mask_value(x, y);
        output(x, y, c) = select(c == 3, s, s + (adjusted - s) * m);

        ms::interleaved(source);
        // (8 x 8 x 2, the two tables next to each other)
        steps.dim(0).set_stride(2);
        steps.dim(2).set_stride(1).set_bounds(0, 2);
        decoder.dim(0).set_stride(2);
        decoder.dim(2).set_stride(1).set_bounds(0, 2);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        ms::interleaved(output);

        // Schedule: the YCbCr samples, each plane's blocks (per block), the chroma doubled,
        // then the output per pixel.
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi"), bxo("bxo"), byo("byo"), bxi("bxi"), byi("byi");
        ycc.compute_root();
        half_cb.compute_root();
        half_cr.compute_root();
        for (Blocks *b : {&bl, &bb, &br}) {
            // (per block: on the GPU, one thread's)
            b->pass1.compute_at(b->inv2, gpu ? bxi : bx);
            b->pass2.compute_at(b->inv2, gpu ? bxi : bx);
            b->inv1.compute_at(b->inv2, gpu ? bxi : bx);
            b->inv2.compute_root().bound(k, 0, 8);
            b->pass1.bound(k, 0, 8);
            b->pass2.bound(u, 0, 8);
            b->inv1.bound(u, 0, 8);
            b->out.compute_root();
        }
        cb_up.first.compute_root();
        cr_up.first.compute_root();
        if (gpu) {
            ycc.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            half_cb.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            half_cr.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            for (Blocks *b : {&bl, &bb, &br}) {
                b->inv2.reorder(k, bx, by).gpu_tile(bx, by, bxo, byo, bxi, byi, 8, 8);
                b->pass1.unroll(k);
                b->pass2.unroll(u);
                b->inv1.unroll(u);
                b->inv2.unroll(k);
                // (the errors apart, a loop per block: inlined in the unrolled DCTs, their
                // hashes 64 times over made the kernel slow to compile, ~10 s at load)
                b->error.compute_root().bound(u, 0, 8).bound(v, 0, 8).reorder(u, v, bx, by);
                b->error.gpu_tile(bx, by, bxo, byo, bxi, byi, 8, 8);
                b->out.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            }
            cb_up.first.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            cr_up.first.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            ycc.vectorize(x, 8).parallel(y);
            half_cb.vectorize(x, 8).parallel(y);
            half_cr.vectorize(x, 8).parallel(y);
            for (Blocks *b : {&bl, &bb, &br}) {
                b->inv2.reorder(k, bx, by).vectorize(bx, 8).parallel(by);
                b->pass1.vectorize(bx, 8);
                b->pass2.vectorize(bx, 8);
                b->inv1.vectorize(bx, 8);
                b->out.vectorize(x, 8).parallel(y);
            }
            cb_up.first.vectorize(x, 8).parallel(y);
            cr_up.first.vectorize(x, 8).parallel(y);
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(JpegDamage, ms_jpeg_damage)
