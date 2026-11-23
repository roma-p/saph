// One stage of a fast Fourier transform along x or y, for the effects that convolve by FFT
// (effects/fft.c runs the stages): a mixed-radix Stockham stage, radix 2 to 32. A transform
// of length n = R_1 x R_2 x ... is its stages in turn, each with its radix R and ns the
// product of the radices before it:
//   out[b ns R + q ns + k] = sum over t < R of in[b ns + k + t n / R] w^(t (k + q ns))
// with w = exp(sign 2 pi i / (ns R)): after the last stage, out is the transform in order.
//
// Images: 4 floats per pixel, two complex numbers (re, im, re, im), interleaved.

#include "Halide.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

class FftStage : public Generator<FftStage> {
  public:
    Input<Buffer<float, 3>> in{"in"};
    Input<int> along_y{"along_y"};
    Input<int> radix{"radix"}; // 2 to 32
    Input<int> ns{"ns"};       // the product of the radices of the stages before
    Input<float> sign{"sign"}; // -1: forward, 1: inverse (not scaled)
    Output<Buffer<float, 3>> out{"out"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr n = select(along_y != 0, in.height(), in.width());
        Expr i = select(along_y != 0, y, x);
        Expr span = ns * radix;
        Expr b = i / span, r = i % span, q = r / ns, k = r % ns;
        Expr j = b * ns + k;
        RDom t(0, radix, "t");
        Expr at = j + t * (n / radix);
        Expr xs = select(along_y != 0, x, at), ys = select(along_y != 0, at, y);
        // The twiddles w^e, e < ns R, in a table (computed once, read from the cache); the
        // exponent reduced exactly in integers first.
        Var ev("e");
        Func twiddle("twiddle");
        Expr angle = sign * (2.0f * float(M_PI)) * cast<float>(ev) / cast<float>(span);
        twiddle(ev) = Tuple(cos(angle), sin(angle));
        Expr e = clamp((t * (k + q * ns)) % span, 0, span - 1);
        Expr cs = twiddle(e)[0], sn = twiddle(e)[1];
        Func acc("acc");
        acc(x, y) = Tuple(0.0f, 0.0f, 0.0f, 0.0f);
        Tuple a = acc(x, y);
        acc(x, y) = Tuple(a[0] + in(xs, ys, 0) * cs - in(xs, ys, 1) * sn,
                          a[1] + in(xs, ys, 0) * sn + in(xs, ys, 1) * cs,
                          a[2] + in(xs, ys, 2) * cs - in(xs, ys, 3) * sn,
                          a[3] + in(xs, ys, 2) * sn + in(xs, ys, 3) * cs);
        Tuple v = acc(x, y);
        out(x, y, c) = mux(c, {v[0], v[1], v[2], v[3]});

        ms::interleaved(in);
        ms::interleaved(out);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        out.bound(c, 0, 4).reorder(c, x, y);
        twiddle.compute_root();
        if (gpu) {
            Var eo("eo"), ei("ei");
            twiddle.gpu_tile(ev, eo, ei, 256);
            out.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
            acc.compute_at(out, xi);
        } else {
            out.unroll(c).parallel(y);
            acc.compute_at(out, x);
        }
    }
};

HALIDE_REGISTER_GENERATOR(FftStage, ms_fft_stage)
