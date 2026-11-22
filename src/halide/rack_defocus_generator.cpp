// MS_RackDefocus in Halide: a lens blur by FFT (effects/ms_rackdefocus.c runs the kernels,
// effects/fft.c the transforms with fft_generator.cpp):
//   ms_rack_defocus_prepare   the Source padded by the Edge Mode into the transform's size,
//                             Chroma Distort's five zooms, Mono's luma, Use Gamma, Boost
//                             Highlights (by the largest channel)
//   ms_rack_defocus_soften    Gauss Blur on the prepared Source, then Use Gamma and Boost
//                             Highlights
//   ms_rack_defocus_kernel    the lens: its shape's coverage of each pixel (turned by Rotate),
//                             Bokeh, Color Fringing (a radius per channel), Lens Noise (Gauss
//                             Blur for Show Shape); summing to 1, centred on pixel (0, 0) of the transform
//                             (wrapped around)
//   ms_rack_defocus_multiply  the product of the two transforms, each holding two real
//                             images (R + i G, B + i A) separated first
//   ms_rack_defocus_finish    back into the image: inverse gamma, Brightness, Offset Darks,
//                             Mix With Source, the Mask
//   ms_rack_defocus_show      Show Shape: the lens in the middle of the image, at most 1
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first; the transforms
// 4 floats per pixel (two complex numbers).

#include "Halide.h"
#include "blur.h"
#include "noise.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

// Use Gamma g: v^g between 0 and 1, v elsewhere (and its inverse with 1 / g).
Expr gamma_of(Expr v, Expr g) { return select(v > 0.0f && v < 1.0f, pow(v, g), v); }

// Use Gamma, then Boost Highlights: the colours boosted by how far their largest is above the
// threshold (after the gamma), all three by the same factor; the alpha as it is.
Func toned(Func s, Expr gamma, Expr boost, Expr threshold) {
    Func v("v"), out("toned");
    v(x, y, c) = gamma_of(s(x, y, c), gamma);
    Expr top = max(v(x, y, 0), v(x, y, 1), v(x, y, 2));
    out(x, y, c) =
        select(c == 3, s(x, y, 3), v(x, y, c) * (1.0f + boost * max(top - threshold, 0.0f)));
    return out;
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
    // Chroma Distort: the Source zoomed about the image's middle by five factors (a spectrum,
    // blue to red), each channel a mix of them; zoom 0: none.
    Input<int> zoom{"zoom"};
    Input<int> tone{"tone"}; // 1: Use Gamma and Boost Highlights here
    Input<float> zoom0{"zoom0"}, zoom1{"zoom1"}, zoom2{"zoom2"}, zoom3{"zoom3"}, zoom4{"zoom4"};
    Output<Buffer<float, 3>> data{"data"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr w = source.width(), h = source.height();
        Func zeros = BoundaryConditions::constant_exterior(source, 0.0f, {{0, w}, {0, h}});
        // Each zoom samples the Source bilinearly (black outside it) about the frame's middle
        // (a pixel corner); the channels' shares of the five (measured): red 0.4476 and 0.5524
        // of the two widest, green (1, sqrt 3, 1) / (2 + sqrt 3) of the middle three, blue
        // the red's mirrored, alpha a fifth of each.
        Expr factors[5] = {zoom0, zoom1, zoom2, zoom3, zoom4};
        const float shares[4][5] = {{0, 0, 0, 0.4476f, 0.5524f},
                                    {0, 0.267949f, 0.464102f, 0.267949f, 0},
                                    {0.5524f, 0.4476f, 0, 0, 0},
                                    {0.2f, 0.2f, 0.2f, 0.2f, 0.2f}};
        Expr mid_x = cast<float>(w) * 0.5f, mid_y = cast<float>(h) * 0.5f;
        Func zoomed("zoomed");
        Expr sum = 0.0f;
        for (int k = 0; k < 5; ++k) {
            Expr px = mid_x + (cast<float>(x) + 0.5f - mid_x) * factors[k] - 0.5f;
            Expr py = mid_y + (cast<float>(y) + 0.5f - mid_y) * factors[k] - 0.5f;
            Expr ix = cast<int>(floor(px)), iy = cast<int>(floor(py));
            Expr fx = px - cast<float>(ix), fy = py - cast<float>(iy);
            Expr v = lerp(lerp(zeros(ix, iy, c), zeros(ix + 1, iy, c), fx),
                          lerp(zeros(ix, iy + 1, c), zeros(ix + 1, iy + 1, c), fx), fy);
            Expr share = select(c == 0, shares[0][k], c == 1, shares[1][k], c == 2,
                                shares[2][k], shares[3][k]);
            sum += select(share != 0.0f, share * v, 0.0f);
        }
        zoomed(x, y, c) = select(zoom != 0, sum, source(clamp(x, 0, w - 1), clamp(y, 0, h - 1), c));
        Func repeat = BoundaryConditions::repeat_edge(zoomed, {{0, w}, {0, h}});
        Func mirror = BoundaryConditions::mirror_interior(zoomed, {{0, w}, {0, h}});
        Func zero = BoundaryConditions::constant_exterior(zoomed, 0.0f, {{0, w}, {0, h}});
        Func s("s");
        Expr sx = x - pad_x, sy = y - pad_y;
        s(x, y, c) = select(edge_mode == 0, zero(sx, sy, c), edge_mode == 1, repeat(sx, sy, c),
                            mirror(sx, sy, c));
        Expr luma = 0.3125f * s(x, y, 0) + 0.5f * s(x, y, 1) + 0.1875f * s(x, y, 2);
        Func m("m");
        m(x, y, c) = select(mono != 0 && c < 3, luma, s(x, y, c));
        // tone 0: Gauss Blur's turn first (ms_rack_defocus_soften then gamma and boost).
        Func t = toned(m, gamma, boost, threshold);
        data(x, y, c) = select(tone != 0, t(x, y, c), m(x, y, c));

        ms::interleaved(source);
        ms::interleaved(data);
        ms::schedule_float4(data, gpu, 32, 8);
    }
};

// Gauss Blur, before Use Gamma and Boost Highlights (they see the softened image): the
// prepared Source (ms_rack_defocus_prepare without them) blurred by blur.h's 3 boxes, then
// toned.
class RackDefocusSoften : public Generator<RackDefocusSoften> {
  public:
    Input<Buffer<float, 3>> prepared{"prepared"};
    Input<int> gauss{"gauss"}; // total radius in whole pixels
    Input<float> gamma{"gamma"};
    Input<float> boost{"boost"};
    Input<float> threshold{"threshold"};
    Output<Buffer<float, 3>> data{"data"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func in("in");
        in(x, y, c) = prepared(x, y, c);
        std::vector<ms_blur::Box> boxes;
        Func blurred = ms_blur::blur3(in, gauss, gauss, prepared.width(), prepared.height(), gpu,
                                      "soften", boxes);
        Func t = toned(blurred, gamma, boost, threshold);
        data(x, y, c) = t(x, y, c);

        ms::interleaved(prepared);
        ms::interleaved(data);
        ms_blur::schedule(boxes, gpu, 4);
        ms::schedule_float4(data, gpu, 32, 8);
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
    // Rotate: the lens turned by this angle (cos, sin; counterclockwise), before Rel Height
    Input<float> turn_cos{"turn_cos"}, turn_sin{"turn_sin"};
    // Lens Noise: each pixel's coverage x max(0, 1 - noise_amp x max(0, fbm)), the fbm of
    // `noise_octaves` (persistence 0.5) at noise_freq cycles per pixel in the lens' frame, at
    // time noise_z (the seed); none with noise_amp 0
    Input<float> noise_amp{"noise_amp"}, noise_freq{"noise_freq"}, noise_z{"noise_z"};
    Input<int> noise_octaves{"noise_octaves"};
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
        // A point of the kernel (y already divided by Rel Height) into the lens' frame: turned
        // back by Rotate.
        auto lens_u = [&](Expr u, Expr v) { return u * turn_cos + v * turn_sin; };
        auto lens_v = [&](Expr u, Expr v) { return v * turn_cos - u * turn_sin; };
        Expr sx = cast<float>(x - half), sy = cast<float>(y - half) / rel_height;
        Expr px = lens_u(sx, sy), py = lens_v(sx, sy);
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
        Expr su = sx + (cast<float>(s.x) + 0.5f) / S - 0.5f;
        Expr sv = sy + ((cast<float>(s.y) + 0.5f) / S - 0.5f) / rel_height;
        Expr u = lens_u(su, sv), v = lens_v(su, sv);
        Expr d = sqrt(u * u + v * v);
        // No radius: the pixel itself (Gauss Blur alone).
        Func cover("cover");
        cover(x, y, c) = select(r <= 0.0f, select(x == half && y == half, 1.0f, 0.0f),
                                whole_in && !sampled, float(S * S) * bokeh_weight(q_mean), 0.0f);
        cover(x, y, c) +=
            select(d <= edge_toward(u, v), bokeh_weight(d * d / max(r * r, 1e-12f)), 0.0f);
        // Lens Noise, at the pixel's centre in the lens' frame.
        Func noisy("noisy");
        Expr n = ms_noise::fbm(px * noise_freq, py * noise_freq, noise_z, noise_octaves, 0.5f, 8,
                               "lens_octave");
        noisy(x, y, c) = cover(x, y, c) *
                         select(noise_amp > 0.0f, max(1.0f - noise_amp * max(n, 0.0f), 0.0f), 1.0f);
        // Gauss Blur: blur.h's 3 boxes, zero outside the lens' square.
        std::vector<ms_blur::Box> boxes;
        Func blurred =
            ms_blur::blur3_subpixel(noisy, gauss, 0.0f, Expr(1), size, size, gpu, "gauss", boxes);
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
        noisy.bound(c, 0, 4).compute_root();
        if (gpu)
            noisy.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            noisy.parallel(y);
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
        ms::schedule_float4(kernel, gpu, 32, 8);
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
        ms::schedule_float4(product, gpu, 32, 8);
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
        ms::schedule_float4(output, gpu, 32, 8);
    }
};

// Show Shape: the lens (ms_rack_defocus_kernel's, wrapped around pixel (0, 0)) centred on
// (cx, cy), each channel divided by its largest value.
class RackDefocusShow : public Generator<RackDefocusShow> {
  public:
    Input<Buffer<float, 3>> lens{"lens"};
    Input<int> cx{"cx"}, cy{"cy"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr nx = lens.width(), ny = lens.height();
        Func largest("largest");
        RDom r(0, nx, 0, ny, "r");
        largest(c) = 0.0f;
        largest(c) = max(largest(c), lens(r.x, r.y, c));
        Expr dx = ((x - cx) % nx + nx) % nx, dy = ((y - cy) % ny + ny) % ny;
        output(x, y, c) = lens(dx, dy, c) / max(largest(c), 1e-30f);

        lens.dim(0).set_stride(4);
        lens.dim(2).set_stride(1).set_bounds(0, 4);
        ms::interleaved(output);
        largest.bound(c, 0, 4).compute_root();
        if (gpu) {
            Var xo("xo"), xi("xi");
            largest.gpu_tile(c, xo, xi, 4);
            largest.update().gpu_tile(c, xo, xi, 4);
        }
        ms::schedule_float4(output, gpu, 32, 8);
    }
};

HALIDE_REGISTER_GENERATOR(RackDefocusPrepare, ms_rack_defocus_prepare)
HALIDE_REGISTER_GENERATOR(RackDefocusSoften, ms_rack_defocus_soften)
HALIDE_REGISTER_GENERATOR(RackDefocusKernel, ms_rack_defocus_kernel)
HALIDE_REGISTER_GENERATOR(RackDefocusMultiply, ms_rack_defocus_multiply)
HALIDE_REGISTER_GENERATOR(RackDefocusFinish, ms_rack_defocus_finish)
HALIDE_REGISTER_GENERATOR(RackDefocusShow, ms_rack_defocus_show)
