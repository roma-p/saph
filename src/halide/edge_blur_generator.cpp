// MS_EdgeBlur in Halide, two kernels (effects/ms_edgeblur.c runs them in order):
//   ms_edge_blur_edges  the edges: the luma (5 R + 8 G + 3 B) / 16 of the Matte (or its alpha),
//                       blurred by Edge Width (blur.h's blur3_subpixel), its gradient's length
//                       (central differences / 2, one-sided at the image's borders) times
//                       `scale`, less Edge Threshold, in [0, 1]
//   ms_edge_blur        the Source blurred by Blur Width, mixed over it by the edges; or the
//                       edges themselves (Show Edges)
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first; the edges one float
// per pixel.

#include "Halide.h"
#include "blur.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

class EdgeBlurEdges : public Generator<EdgeBlurEdges> {
  public:
    Input<Buffer<float, 3>> matte{"matte"}; // the Source when there is no Matte
    Input<int> use_alpha{"use_alpha"};
    Input<int> radius{"radius"}; // Edge Width: whole pixels
    Input<float> frac{"frac"};   // and the fraction past them
    Input<float> scale{"scale"}; // of the gradient
    Input<float> threshold{"threshold"};
    Output<Buffer<float, 2>> edges{"edges"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr width = matte.width(), height = matte.height();
        Func luma("luma");
        luma(x, y, c) =
            select(use_alpha != 0, matte(x, y, 3),
                   0.3125f * matte(x, y, 0) + 0.5f * matte(x, y, 1) + 0.1875f * matte(x, y, 2));
        std::vector<ms_blur::Box> boxes;
        Func b = ms_blur::blur3_subpixel(luma, radius, frac, radius, radius, frac, radius, Expr(0),
                                         width, height, gpu, "edges", boxes);
        // One coordinate's difference: between the neighbours, or this pixel and its only one.
        auto slope = [&](Expr lo_x, Expr lo_y, Expr hi_x, Expr hi_y, Expr steps) {
            return select(steps > 0, (b(hi_x, hi_y, 0) - b(lo_x, lo_y, 0)) / cast<float>(steps),
                          0.0f);
        };
        Expr xl = max(x - 1, 0), xr = min(x + 1, width - 1);
        Expr yl = max(y - 1, 0), yr = min(y + 1, height - 1);
        Expr gx = slope(xl, y, xr, y, xr - xl), gy = slope(x, yl, x, yr, yr - yl);
        edges(x, y) = clamp(scale * sqrt(gx * gx + gy * gy) - threshold, 0.0f, 1.0f);

        ms::interleaved(matte);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 1);
        if (gpu)
            edges.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            edges.vectorize(x, 8).parallel(y);
    }
};

class EdgeBlur : public Generator<EdgeBlur> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> edges{"edges"}; // ms_edge_blur_edges
    Input<int> radius{"radius"};            // Blur Width: whole pixels
    Input<float> frac{"frac"};              // and the fraction past them
    Input<int> show_edges{"show_edges"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func src("src");
        src(x, y, c) = source(x, y, c);
        std::vector<ms_blur::Box> boxes;
        Func b = ms_blur::blur3_subpixel(src, radius, frac, radius, radius, frac, radius, Expr(0),
                                         source.width(), source.height(), gpu, "blur", boxes);
        Expr e = edges(x, y), s = source(x, y, c);
        output(x, y, c) =
            select(show_edges != 0, select(c == 3, 1.0f, e), s + (b(x, y, c) - s) * e);

        ms::interleaved(source);
        ms::interleaved(output);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 4);
        if (gpu)
            output.bound(c, 0, 4).reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16,
                                                                         16);
        else
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
    }
};

HALIDE_REGISTER_GENERATOR(EdgeBlurEdges, ms_edge_blur_edges)
HALIDE_REGISTER_GENERATOR(EdgeBlur, ms_edge_blur)
