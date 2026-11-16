// MillSaph's seeded noise (the effects' randomness: their own, not the reference's):
//   hash3     an integer hash of a lattice point
//   uniform   0..1 from a hash (its 24 high bits), symmetric: -1..1
//   perlin3   gradient noise in 3D (x, y and time, or an angle), about -0.9..0.9, sd 0.28
//   fbm       octaves of perlin3, each twice the frequency and `persistence` x the amplitude
// The same on the CPU and the GPU (32-bit integer hashing, float maths).

#pragma once

#include "Halide.h"

namespace ms_noise {

using namespace Halide;

// A 32-bit unsigned constant.
inline Expr u32(uint64_t v) { return Internal::make_const(UInt(32), v); }

// A 32-bit hash of the lattice point (ix, iy, iz) (any integers: wrapping).
inline Expr hash3(Expr ix, Expr iy, Expr iz) {
    Expr h = (cast<uint32_t>(ix) * u32(0x8da6b343)) ^
             (cast<uint32_t>(iy) * u32(0xd8163841)) ^
             (cast<uint32_t>(iz) * u32(0xcb1ab31f));
    h = (h ^ (h >> u32(16))) * u32(0x7feb352d);
    h = (h ^ (h >> u32(15))) * u32(0x846ca68b);
    return h ^ (h >> u32(16));
}

// 0..1 from a 32-bit hash (its 24 high bits); -1..1.
inline Expr uniform(Expr h) { return cast<float>(h >> u32(8)) * (1.0f / 16777216.0f); }
inline Expr symmetric(Expr h) { return uniform(h) * 2.0f - 1.0f; }

// The dot product of one of Perlin's 12 gradients (by the hash) with (x, y, z).
inline Expr gradient(Expr h, Expr x, Expr y, Expr z) {
    Expr k = cast<int>(h & u32(15));
    Expr u = select(k < 8, x, y);
    Expr v = select(k < 4, y, k == 12 || k == 14, x, z);
    return select((k & 1) != 0, -u, u) + select((k & 2) != 0, -v, v);
}

inline Expr fade(Expr t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Gradient noise at (x, y, z): 0 on the lattice, smooth between.
inline Expr perlin3(Expr x, Expr y, Expr z) {
    Expr fx0 = floor(x), fy0 = floor(y), fz0 = floor(z);
    Expr ix = cast<int>(fx0), iy = cast<int>(fy0), iz = cast<int>(fz0);
    Expr fx = x - fx0, fy = y - fy0, fz = z - fz0;
    auto g = [&](int a, int b, int c) {
        return gradient(hash3(ix + a, iy + b, iz + c), fx - a, fy - b, fz - c);
    };
    Expr u = fade(fx), v = fade(fy), w = fade(fz);
    auto mix = [](Expr a, Expr b, Expr t) { return a + (b - a) * t; };
    return mix(mix(mix(g(0, 0, 0), g(1, 0, 0), u), mix(g(0, 1, 0), g(1, 1, 0), u), v),
               mix(mix(g(0, 0, 1), g(1, 0, 1), u), mix(g(0, 1, 1), g(1, 1, 1), u), v), w);
}

// `octaves` (an Expr, at most max_octaves) of perlin3 at (x, y) x 2^k (each octave shifted
// apart), z the same for all, amplitude persistence^k (an inline sum: only those asked).
inline Expr fbm(Expr x, Expr y, Expr z, Expr octaves, Expr persistence, int max_octaves,
                const std::string &name) {
    RDom k(0, max_octaves, name);
    k.where(k < octaves);
    Expr f = cast<float>(1 << k), kf = cast<float>(k);
    Expr amplitude = select(k == 0, 1.0f, pow(persistence, kf)); // (0^0: 1)
    return sum(amplitude * perlin3(x * f + 17.3f * kf, y * f + 5.1f * kf, z));
}

} // namespace ms_noise
