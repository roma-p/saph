// MS_RackDefocus in Halide: a lens blur by FFT (effects/ms_rackdefocus.c runs the kernels,
// effects/fft.c the transforms with fft_generator.cpp):
//   ms_rack_defocus_prepare   the Source padded by the Edge Mode into the transform's size,
//                             Mono's luma, Use Gamma, Boost Highlights
//   ms_rack_defocus_kernel    the lens: its shape's coverage of each pixel, Bokeh, Color
//                             Fringing (a radius per channel), Gauss Blur; summing to 1,
//                             centred on pixel (0, 0) of the transform (wrapped around)
//   ms_rack_defocus_multiply  the product of the two transforms, each holding two real
//                             images (R + i G, B + i A) separated first
//   ms_rack_defocus_finish    back into the image: inverse gamma, Brightness, Offset Darks,
//                             Mix With Source, the Mask
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first; the transforms
// 4 floats per pixel (two complex numbers).

#include "Halide.h"
#include "blur.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

// Use Gamma g: v^g between 0 and 1, v elsewhere (and its inverse with 1 / g).
Expr gamma_of(Expr v, Expr g) { return select(v > 0.0f && v < 1.0f, pow(v, g), v); }

// The GPU schedule of a whole-image stage: float4 pixels, tiles of threads.
void tiles(Func f, bool gpu) {
    Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
    f.bound(c, 0, 4).reorder(c, x, y);
    if (gpu)
        f.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
    else
        f.unroll(c).vectorize(x, 8).parallel(y);
}

} // namespace

class RackDefocusPrepare : public Generator<RackDefocusPrepare> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<int> pad_x{"pad_x"}, pad_y{"pad_y"}; // the Source's place in the transform
    Input<int> edge_mode{"edge_mode"};         // Transparent, Repeat, Reflect
    Input<int> mono{"mono"};
    Input<float> gamma{"gamma"};
    Input<float> boost{"boost"}; // Boost Highlights' factor
    Input<float> threshold{"threshold"};
    Output<Buffer<float, 3>> data{"data"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr w = source.width(), h = source.height();
        Func zeros = BoundaryConditions::constant_exterior(source, 0.0f, {{0, w}, {0, h}});
        Func repeat = BoundaryConditions::repeat_edge(source, {{0, w}, {0, h}});
        Func mirror = BoundaryConditions::mirror_interior(source, {{0, w}, {0, h}});
        Func s("s");
        Expr sx = x - pad_x, sy = y - pad_y;
        s(x, y, c) = select(edge_mode == 0, zeros(sx, sy, c), edge_mode == 1, repeat(sx, sy, c),
                            mirror(sx, sy, c));
        Expr luma = 0.3125f * s(x, y, 0) + 0.5f * s(x, y, 1) + 0.1875f * s(x, y, 2);
        Expr v = gamma_of(select(mono != 0, luma, s(x, y, c)), gamma);
        // The colours boosted above the threshold (after the gamma); the alpha only blurred.
        data(x, y, c) = select(c == 3, s(x, y, 3), v + boost * v * max(v - threshold, 0.0f));

        ms::interleaved(source);
        ms::interleaved(data);
        tiles(data, gpu);
    }
};

class RackDefocusKernel : public Generator<RackDefocusKernel> {
  public:
    Input<float> radius_r{"radius_r"}, radius_g{"radius_g"}, radius_b{"radius_b"};
    Input<float> rel_height{"rel_height"};
    Input<int> sides{"sides"};   // 0: a circle
    Input<float> power{"power"}; // Roundness: the polygon's radius to this power
    Input<float> bokeh{"bokeh"};
    Input<int> gauss{"gauss"}; // Gauss Blur: total radius in whole pixels
    Input<int> half{"half"};   // the lens within [-half, half] in x and y
    Output<Buffer<float, 3>> kernel{"kernel"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        const int S = 16; // samples per pixel and axis
        Expr size = 2 * half + 1;
        // The lens' coverage of pixel (x - half, y - half), weighted by Bokeh, channels R, G, B:
        // whole inside or outside, from its centre; near the lens' edge (and for a star), the
        // count of S x S samples inside.
        Expr r = select(c == 0, radius_r, c == 2, radius_b, radius_g);
        Expr a = float(M_PI) / cast<float>(max(sides, 1));
        // The lens' edge toward (u, v) (v already divided by Rel Height): the regular polygon,
        // a vertex up (+y), its radius to the power.
        auto edge_toward = [&](Expr u, Expr v) {
            Expr theta = atan2(v, u) - float(M_PI) / 2.0f;
            Expr phase = theta - 2.0f * a * floor(theta / (2.0f * a)); // in [0, 2a)
            return r * pow(cos(a) / cos(phase - a), power);
        };
        auto bokeh_weight = [&](Expr q) { // q = (d / r)^2
            return select(bokeh >= 0.0f, 1.0f - bokeh * q, 1.0f + bokeh - bokeh * q);
        };
        Expr px = cast<float>(x - half), py = cast<float>(y - half) / rel_height;
        Expr dc = sqrt(px * px + py * py), ec = edge_toward(px, py);
        // Half a pixel's diagonal, plus how much a polygon's edge can move across a pixel.
        Expr margin = 2.0f * max(1.0f, 1.0f / rel_height);
        Expr whole_in = power <= 1.0f && dc + margin < ec;
        Expr sampled = power > 1.0f || abs(dc - ec) <= margin;
        // Inside: Bokeh's weight averaged over the pixel (the mean of x^2 over it: x^2 + 1/12).
        Expr q_mean = (px * px + py * py + (1.0f + 1.0f / (rel_height * rel_height)) / 12.0f) /
                      max(r * r, 1e-12f);
        RDom s(0, S, 0, S, "s");
        s.where(r > 0.0f && sampled);
        Expr u = px + (cast<float>(s.x) + 0.5f) / S - 0.5f;
        Expr v = py + ((cast<float>(s.y) + 0.5f) / S - 0.5f) / rel_height;
        Expr d = sqrt(u * u + v * v);
        // No radius: the pixel itself (Gauss Blur alone).
        Func cover("cover");
        cover(x, y, c) = select(r <= 0.0f, select(x == half && y == half, 1.0f, 0.0f),
                                whole_in && !sampled, float(S * S) * bokeh_weight(q_mean), 0.0f);
        cover(x, y, c) +=
            select(d <= edge_toward(u, v), bokeh_weight(d * d / max(r * r, 1e-12f)), 0.0f);
        // Gauss Blur: blur.h's 3 boxes, zero outside the lens' square.
        std::vector<ms_blur::Box> boxes;
        Func blurred = ms_blur::blur3_subpixel(cover, gauss, 0.0f, gauss, gauss, 0.0f, gauss,
                                               Expr(1), size, size, gpu, "gauss", boxes);
        // Its sum: per row, then the rows.
        Func rows("rows"), total("total");
        RDom rx(0, size, "rx"), ry(0, size, "ry");
        rows(y, c) = 0.0f;
        rows(y, c) += blurred(rx, y, c);
        total(c) = 0.0f;
        total(c) += rows(ry, c);
        // Wrapped around pixel (0, 0).
        Expr nx = kernel.width(), ny = kernel.height();
        Expr dx = select(x <= nx / 2, x, x - nx), dy = select(y <= ny / 2, y, y - ny);
        Expr ch = select(c == 3, 1, c);
        Expr inside = abs(dx) <= half && abs(dy) <= half;
        Expr sum = total(ch);
        Expr value = blurred(clamp(dx + half, 0, size - 1), clamp(dy + half, 0, size - 1), ch);
        kernel(x, y, c) = select(inside, value / sum, 0.0f);

        kernel.dim(0).set_stride(4);
        kernel.dim(2).set_stride(1).set_bounds(0, 4);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        cover.bound(c, 0, 4).compute_root(); // R, G, B (and G again: whole float4)
        if (gpu) {
            cover.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            cover.update().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            cover.parallel(y);
            cover.update().reorder(s.x, s.y, x, y, c).parallel(y);
        }
        ms_blur::schedule(boxes, gpu, 4);
        rows.compute_root();
        total.compute_root();
        if (gpu) {
            rows.gpu_tile(y, xo, xi, 64);
            rows.update().gpu_tile(y, xo, xi, 64);
            total.gpu_single_thread();
            total.update().gpu_single_thread();
        } else {
            rows.update().parallel(y);
        }
        tiles(kernel, gpu);
    }
};

class RackDefocusMultiply : public Generator<RackDefocusMultiply> {
  public:
    Input<Buffer<float, 3>> image{"image"}; // transform of the prepared Source
    Input<Buffer<float, 3>> lens{"lens"};   // transform of the lens
    Output<Buffer<float, 3>> product{"product"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr nx = image.width(), ny = image.height();
        // (-k) wrapped; clamped for Halide's bounds (its select keeps both sides).
        Expr xm = select(x == 0, 0, clamp(nx - x, 1, nx - 1));
        Expr ym = select(y == 0, 0, clamp(ny - y, 1, ny - 1));
        // The transforms of the two real images packed as p + i q, at frequency k from Z(k)
        // and Z(-k): P = (Z(k) + conj Z(-k)) / 2, Q = (Z(k) - conj Z(-k)) / 2i.
        auto parts = [&](Func z, int plane) {
            Expr re = z(x, y, 2 * plane), im = z(x, y, 2 * plane + 1);
            Expr rm = z(xm, ym, 2 * plane), imm = z(xm, ym, 2 * plane + 1);
            return std::vector<Expr>{(re + rm) * 0.5f, (im - imm) * 0.5f, (im + imm) * 0.5f,
                                     (rm - re) * 0.5f};
        };
        Func img("img"), krn("krn");
        img(x, y, c) = image(x, y, c);
        krn(x, y, c) = lens(x, y, c);
        std::vector<Expr> out;
        for (int plane = 0; plane < 2; ++plane) {
            std::vector<Expr> f = parts(img, plane), k = parts(krn, plane);
            // P_image P_lens + i Q_image Q_lens.
            Expr pr = f[0] * k[0] - f[1] * k[1], pi = f[0] * k[1] + f[1] * k[0];
            Expr qr = f[2] * k[2] - f[3] * k[3], qi = f[2] * k[3] + f[3] * k[2];
            out.push_back(pr - qi);
            out.push_back(pi + qr);
        }
        product(x, y, c) = mux(c, out);

        for (auto *b : {&image, &lens}) {
            (*b).dim(0).set_stride(4);
            (*b).dim(2).set_stride(1).set_bounds(0, 4);
        }
        product.dim(0).set_stride(4);
        product.dim(2).set_stride(1).set_bounds(0, 4);
        tiles(product, gpu);
    }
};

class RackDefocusFinish : public Generator<RackDefocusFinish> {
  public:
    Input<Buffer<float, 3>> data{"data"}; // the inverse transform, not scaled
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // 1 everywhere without a Mask
    Input<int> pad_x{"pad_x"}, pad_y{"pad_y"};
    Input<float> scale{"scale"}; // 1 / the transform's size
    Input<float> gamma{"gamma"};
    Input<float> brightness{"brightness"}, offset_darks{"offset_darks"}, mix{"mix"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr b = data(x + pad_x, y + pad_y, c) * scale;
        Expr bg = gamma_of(b, 1.0f / gamma);
        Expr rgb = brightness * bg + offset_darks * (1.0f - bg);
        Expr blurred = select(c == 3, b, rgb);
        Expr s = source(x, y, c);
        Expr mixed = blurred + (s - blurred) * mix;
        output(x, y, c) = s + (mixed - s) * mask_value(x, y);

        data.dim(0).set_stride(4);
        data.dim(2).set_stride(1).set_bounds(0, 4);
        ms::interleaved(source);
        ms::interleaved(output);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        tiles(output, gpu);
    }
};

HALIDE_REGISTER_GENERATOR(RackDefocusPrepare, ms_rack_defocus_prepare)
HALIDE_REGISTER_GENERATOR(RackDefocusKernel, ms_rack_defocus_kernel)
HALIDE_REGISTER_GENERATOR(RackDefocusMultiply, ms_rack_defocus_multiply)
HALIDE_REGISTER_GENERATOR(RackDefocusFinish, ms_rack_defocus_finish)
