// The test images of the benchmark, and the uncompressed EXR files they go through. See
// images.h.

// strnlen: hidden by glibc in strict C11 without it.
#define _POSIX_C_SOURCE 200809L

#include "images.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- noise

// A value in [0, 1) for each lattice point and seed.
static float hash01(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f;
}

// Value noise: lattice values every `cell` pixels, smoothly interpolated.
static float value_noise(float x, float y, float cell, uint32_t seed) {
    const float u = x / cell, v = y / cell;
    const float fu = floorf(u), fv = floorf(v);
    float tu = u - fu, tv = v - fv;
    tu = tu * tu * (3 - 2 * tu);
    tv = tv * tv * (3 - 2 * tv);
    const uint32_t i = (uint32_t)(int32_t)fu, j = (uint32_t)(int32_t)fv;
    const float a = hash01(i, j, seed), b = hash01(i + 1, j, seed);
    const float c = hash01(i, j + 1, seed), d = hash01(i + 1, j + 1, seed);
    return (a + (b - a) * tu) + ((c + (d - c) * tu) - (a + (b - a) * tu)) * tv;
}

// Fractal noise in [0, 1): octaves from a quarter of the width down to 2 pixels, each
// `persistence` times as strong as the one before (0.5: the 1/f spectrum of natural images;
// more: rougher, like foliage).
static float fractal_p(float x, float y, int width, uint32_t seed, float persistence) {
    float sum = 0, amplitude = 0.5f, total = 0;
    for (float cell = (float)width / 4; cell >= 2; cell /= 2) {
        sum += amplitude * value_noise(x, y, cell, seed++);
        total += amplitude;
        amplitude *= persistence;
    }
    return sum / total;
}

static float fractal(float x, float y, int width, uint32_t seed) {
    return fractal_p(x, y, width, seed, 0.5f);
}

// Film grain: about Gaussian (sum of 4 uniform values), mean 0, deviation `sigma`.
static float grain(int x, int y, int k, float sigma) {
    float s = 0;
    for (uint32_t n = 0; n < 4; ++n)
        s += hash01((uint32_t)x, (uint32_t)y, 1000u + (uint32_t)k * 4u + n);
    return (s - 2.0f) * sigma * 1.732f;
}

#define AT(p, w, x, y) ((p) + ((size_t)(y) * (w) + (x)) * 4)

// ---------------------------------------------------------------- images

// Gradients, a disc and a box with hard edges, a little noise (the sweep's own image, as
// tools/test_host.c --size).
static void make_reference(float *p, int width, int height) {
    unsigned seed = 1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = 0; k < 4; ++k) {
                float v = 1.0f;
                if (k < 3) {
                    seed = seed * 1664525u + 1013904223u;
                    const float noise = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.04f;
                    const float dx = (float)x / (float)width - 0.39f;
                    const float dy = (float)y / (float)height - 0.46f;
                    if (dx * dx * 256.0f / 81.0f + dy * dy < 0.077f)
                        v = 0.9f - 0.3f * (float)k + noise;
                    else if (x > width * 68 / 100 && x < width * 88 / 100 && y > height / 7 &&
                             y < height * 5 / 6)
                        v = 0.1f + 0.2f * (float)k + noise;
                    else
                        v = 0.2f + 0.5f * (float)x / (float)width * (float)(k + 1) / 3.0f +
                            0.2f * (float)y / (float)height + noise;
                }
                AT(p, width, x, y)[k] = v;
            }
}

// Like a plate: fractal light and colour; a rough textured ground (like foliage) on the
// lower third; objects with hard edges (a dark box, 40 discs and boxes of every size) and a
// bright soft-edged blob; grain growing with the light; a few highlights above 1.
static void make_plate(float *p, int width, int height) {
    // The objects: centre, size, kind (disc or box), brightness; fixed by their index.
    enum { OBJECTS = 40 };
    float ox[OBJECTS], oy[OBJECTS], size[OBJECTS], gain[OBJECTS];
    int disc[OBJECTS];
    for (int i = 0; i < OBJECTS; ++i) {
        ox[i] = hash01((uint32_t)i, 0, 200);
        oy[i] = hash01((uint32_t)i, 1, 200);
        size[i] = 0.005f + 0.08f * hash01((uint32_t)i, 2, 200) * hash01((uint32_t)i, 3, 200);
        gain[i] = 0.3f + 1.6f * hash01((uint32_t)i, 4, 200);
        disc[i] = hash01((uint32_t)i, 5, 200) < 0.5f;
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float fx = (float)x, fy = (float)y;
            const float u = fx / (float)width, v = fy / (float)height;
            float light = 0.03f + 0.55f * fractal(fx, fy, width, 10);
            if (v < 0.33f) // the ground, rough
                light = 0.02f + 0.4f * fractal_p(fx, fy, width, 20, 0.8f);
            const float warm = fractal(fx, fy, width, 30) - 0.5f;
            const float green = fractal(fx, fy, width, 50) - 0.5f;
            if (u > 0.12f && u < 0.31f && v > 0.2f && v < 0.7f)
                light *= 0.25f;
            for (int i = 0; i < OBJECTS; ++i) {
                const float du = (u - ox[i]) * (float)width / (float)height, dv = v - oy[i];
                if (disc[i] ? du * du + dv * dv < size[i] * size[i]
                            : fabsf(du) < size[i] && fabsf(dv) < size[i] * 0.6f)
                    light *= gain[i];
            }
            const float du = u - 0.62f, dv = v - 0.55f;
            light += 0.6f * expf(-(du * du * 3.16f + dv * dv) * 40.0f);
            float rgb[3] = {light * (1 + 0.6f * warm), light * (1 + 0.4f * green),
                            light * (1 - 0.6f * warm)};
            if (v < 0.33f) { // greener ground
                rgb[0] *= 0.7f;
                rgb[2] *= 0.6f;
            }
            // Highlights: small bright spots.
            if (hash01((uint32_t)x / 6, (uint32_t)y / 6, 70) > 0.9993f)
                rgb[0] = rgb[1] = rgb[2] = 4 + 4 * hash01((uint32_t)x / 6, (uint32_t)y / 6, 71);
            float *o = AT(p, width, x, y);
            for (int k = 0; k < 3; ++k)
                o[k] = rgb[k] + grain(x, y, k, 0.012f * sqrtf(rgb[k] > 0 ? rgb[k] : 0));
            o[3] = 1.0f;
        }
}

static void make_flat(float *p, int width, int height) {
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        p[i * 4 + 0] = p[i * 4 + 1] = p[i * 4 + 2] = 0.18f;
        p[i * 4 + 3] = 1.0f;
    }
}

static void make_noise(float *p, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = 0; k < 4; ++k)
                AT(p, width, x, y)[k] = k < 3 ? hash01((uint32_t)x, (uint32_t)y, 90u + k) : 1.0f;
}

// Values too small for normal floats (denormals, below 1.2e-38): slow on some CPUs.
static void make_denormal(float *p, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float *o = AT(p, width, x, y);
            for (int k = 0; k < 3; ++k)
                o[k] = fractal((float)x, (float)y, width, 110u + (uint32_t)k) * 1e-39f;
            o[3] = 1.0f;
        }
}

// High dynamic range: a sky from 1 to 20, a sun of 500, bright speculars, dark ground.
static void make_hdr(float *p, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float u = (float)x / (float)width, v = (float)y / (float)height;
            float l = v > 0.4f ? 1 + 19 * (v - 0.4f) / 0.6f : 0.02f + 0.1f * fractal((float)x, (float)y, width, 130);
            const float du = u - 0.7f, dv = v - 0.8f;
            if (du * du * 3.16f + dv * dv < 0.0004f)
                l = 500;
            if (v < 0.4f && hash01((uint32_t)x / 3, (uint32_t)y / 3, 140) > 0.999f)
                l = 50 + 50 * hash01((uint32_t)x / 3, (uint32_t)y / 3, 141);
            float *o = AT(p, width, x, y);
            o[0] = l;
            o[1] = l * 0.95f;
            o[2] = l * (v > 0.4f ? 1.1f : 0.8f);
            o[3] = 1.0f;
        }
}

// The plate premultiplied by an alpha: 0 over the left third, a disc of 1 with a soft edge
// to 0.6 elsewhere.
static void make_alpha(float *p, int width, int height) {
    make_plate(p, width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float u = (float)x / (float)width, v = (float)y / (float)height;
            const float du = u - 0.6f, dv = v - 0.5f;
            const float r = sqrtf(du * du * 3.16f + dv * dv);
            float a = u < 0.33f   ? 0.0f
                      : r < 0.2f  ? 1.0f
                      : r < 0.3f  ? 0.6f + 0.4f * (0.3f - r) / 0.1f
                                  : 0.6f;
            float *o = AT(p, width, x, y);
            for (int k = 0; k < 3; ++k)
                o[k] *= a;
            o[3] = a;
        }
}

// The reference with a grid of NaN and infinite pixels (broken renders upstream).
static void make_nan_inf(float *p, int width, int height) {
    make_reference(p, width, height);
    for (int y = 0; y < height; y += 97)
        for (int x = 0; x < width; x += 89) {
            float *o = AT(p, width, x, y);
            const float bad = (x / 89 + y / 97) % 2 ? NAN : INFINITY;
            o[0] = o[1] = o[2] = bad;
        }
}

// A Mask above 1 (an HDR matte: 0 to 4 across the width): larger windows in EdgeAwareBlur.
static void make_mask_hdr(float *p, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float *o = AT(p, width, x, y);
            o[0] = o[1] = o[2] = o[3] = 4.0f * (float)x / (float)(width - 1);
        }
}

// A Mask black but for a small soft disc (5 % of the frame): the effect is needed there only.
static void make_mask_sparse(float *p, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float du = (float)x / (float)width - 0.3f, dv = (float)y / (float)height - 0.4f;
            const float r = sqrtf(du * du * 3.16f + dv * dv);
            const float m = r < 0.1f ? 1.0f : r < 0.13f ? (0.13f - r) / 0.03f : 0.0f;
            float *o = AT(p, width, x, y);
            o[0] = o[1] = o[2] = o[3] = m;
        }
}

const MsTestImage ms_test_images[] = {
    {"reference", "gradients, a disc and a box, a little noise (the parameter sweep's image)", 0, 0,
     make_reference},
    {"plate", "like a plate: fractal light and colour, hard and soft edges, grain, highlights", 0,
     0, make_plate},
    {"flat", "one grey (0.18) everywhere", 0, 0, make_flat},
    {"noise", "white noise, 0 to 1 per channel", 0, 0, make_noise},
    {"denormal", "values below 1e-38 (denormal floats)", 0, 1, make_denormal},
    {"hdr", "sky 1 to 20, a sun of 500, speculars up to 100", 0, 0, make_hdr},
    {"alpha", "the plate premultiplied: alpha 0 on a third, a soft disc", 0, 0, make_alpha},
    {"nan_inf", "the reference with a grid of NaN and infinite pixels", 0, 0, make_nan_inf},
    {"mask_hdr", "Mask from 0 to 4 across the frame", 1, 0, make_mask_hdr},
    {"mask_sparse", "Mask black but for a soft disc (5 % of the frame)", 1, 0, make_mask_sparse},
};
const int ms_test_image_count = (int)(sizeof ms_test_images / sizeof ms_test_images[0]);

// ---------------------------------------------------------------- EXR

// Half floats (IEEE 754 binary16), rounded to the nearest.
static uint16_t to_half(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    const uint16_t sign = (uint16_t)((u >> 16) & 0x8000u);
    const uint32_t a = u & 0x7fffffffu;
    if (a >= 0x7f800000u) // infinity, NaN
        return (uint16_t)(sign | 0x7c00u | (a > 0x7f800000u ? 0x200u : 0));
    if (a >= 0x477ff000u) // too large: infinity
        return (uint16_t)(sign | 0x7c00u);
    if (a < 0x33000000u) // too small: 0
        return sign;
    if (a < 0x38800000u) { // a half denormal
        const uint32_t mant = (a & 0x7fffffu) | 0x800000u;
        const int shift = 126 - (int)(a >> 23);
        const uint32_t h = mant >> (shift + 1);
        const uint32_t rest = mant & ((1u << (shift + 1)) - 1), halfway = 1u << shift;
        return (uint16_t)(sign | (h + (rest > halfway || (rest == halfway && (h & 1)))));
    }
    const uint32_t h = ((a - 0x38000000u) >> 13), rest = a & 0x1fffu;
    return (uint16_t)(sign | (h + (rest > 0x1000u || (rest == 0x1000u && (h & 1)))));
}

static float from_half(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1fu, m = h & 0x3ffu, u;
    if (e == 0 && m == 0)
        u = sign;
    else if (e == 0) { // denormal
        e = 113;
        while (!(m & 0x400u)) {
            m <<= 1;
            --e;
        }
        u = sign | (e << 23) | ((m & 0x3ffu) << 13);
    } else if (e == 31)
        u = sign | 0x7f800000u | (m << 13);
    else
        u = sign | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void put32(FILE *f, uint32_t v) {
    const unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                                (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put_attribute(FILE *f, const char *name, const char *type, const void *value,
                          uint32_t size) {
    fwrite(name, 1, strlen(name) + 1, f);
    fwrite(type, 1, strlen(type) + 1, f);
    put32(f, size);
    fwrite(value, 1, size, f);
}

// Little-endian values for the attributes (the machines here are all little-endian).
int ms_exr_write(const char *path, const float *pixels, int width, int height, int full_float) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    put32(f, 20000630u); // magic
    put32(f, 2u);        // version 2, single-part scanline
    // Channels, in alphabetical order: A, B, G, R; their place in our pixels: 3, 2, 1, 0.
    unsigned char chlist[4 * 18 + 1];
    const char *names = "ABGR";
    size_t n = 0;
    const int32_t type = full_float ? 2 : 1, sampling = 1;
    for (int c = 0; c < 4; ++c) {
        chlist[n++] = (unsigned char)names[c];
        chlist[n++] = 0;
        memcpy(chlist + n, &type, 4);
        n += 4;
        memset(chlist + n, 0, 4); // pLinear, reserved
        n += 4;
        memcpy(chlist + n, &sampling, 4);
        n += 4;
        memcpy(chlist + n, &sampling, 4);
        n += 4;
    }
    chlist[n++] = 0;
    put_attribute(f, "channels", "chlist", chlist, (uint32_t)n);
    const unsigned char none = 0, increasing_y = 0;
    put_attribute(f, "compression", "compression", &none, 1);
    const int32_t window[4] = {0, 0, width - 1, height - 1};
    put_attribute(f, "dataWindow", "box2i", window, 16);
    put_attribute(f, "displayWindow", "box2i", window, 16);
    put_attribute(f, "lineOrder", "lineOrder", &increasing_y, 1);
    const float one = 1.0f, center[2] = {0, 0};
    put_attribute(f, "pixelAspectRatio", "float", &one, 4);
    put_attribute(f, "screenWindowCenter", "v2f", center, 8);
    put_attribute(f, "screenWindowWidth", "float", &one, 4);
    fputc(0, f); // end of the header
    // The line offsets, then the lines: y, size, then each channel's values along the line.
    const size_t bytes = (size_t)(full_float ? 4 : 2), line = (size_t)width * 4 * bytes;
    const long table = ftell(f);
    const uint64_t first = (uint64_t)table + 8u * (uint64_t)height;
    for (int y = 0; y < height; ++y) {
        const uint64_t at = first + (uint64_t)y * (8 + line);
        put32(f, (uint32_t)at);
        put32(f, (uint32_t)(at >> 32));
    }
    unsigned char *data = malloc(line);
    int ok = data != NULL;
    for (int y = 0; ok && y < height; ++y) {
        // EXR lines go from the top; our pixels from the bottom.
        const float *row = pixels + (size_t)(height - 1 - y) * width * 4;
        for (int c = 0; c < 4; ++c)
            for (int x = 0; x < width; ++x) {
                const float v = row[(size_t)x * 4 + (3 - c)];
                unsigned char *to = data + ((size_t)c * width + x) * bytes;
                if (full_float) {
                    memcpy(to, &v, 4);
                } else {
                    const uint16_t h = to_half(v);
                    memcpy(to, &h, 2);
                }
            }
        put32(f, (uint32_t)y);
        put32(f, (uint32_t)line);
        ok = fwrite(data, 1, line, f) == line;
    }
    free(data);
    return fclose(f) == 0 && ok;
}

static int fail(char *err, size_t size, const char *why, FILE *f) {
    snprintf(err, size, "%s", why);
    if (f)
        fclose(f);
    return 0;
}

int ms_exr_read(const char *path, float *pixels, int width, int height, char *err,
                size_t err_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(err, err_size, "cannot open it", NULL);
    unsigned char head[8];
    if (fread(head, 1, 8, f) != 8 || head[0] != 0x76 || head[1] != 0x2f || head[2] != 0x31 ||
        head[3] != 0x01)
        return fail(err, err_size, "not an EXR file", f);
    if (head[5] & 0x1a) // tiled, deep or multi-part
        return fail(err, err_size, "tiled or multi-part EXR: save it as scanlines", f);
    // The attributes this reader needs.
    char names[16][32];
    int32_t types[16];
    int channels = 0, compression = -1;
    int32_t window[4] = {0, 0, -1, -1};
    for (;;) {
        char name[256], type[256];
        int c, i = 0;
        while ((c = fgetc(f)) > 0 && i < 255)
            name[i++] = (char)c;
        name[i] = 0;
        if (c < 0)
            return fail(err, err_size, "truncated header", f);
        if (i == 0)
            break; // end of the header
        i = 0;
        while ((c = fgetc(f)) > 0 && i < 255)
            type[i++] = (char)c;
        type[i] = 0;
        uint32_t size;
        unsigned char b[4];
        if (c < 0 || fread(b, 1, 4, f) != 4)
            return fail(err, err_size, "truncated header", f);
        size = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        unsigned char *value = malloc(size ? size : 1);
        if (!value || fread(value, 1, size, f) != size) {
            free(value);
            return fail(err, err_size, "truncated header", f);
        }
        if (strcmp(name, "channels") == 0) {
            size_t at = 0;
            while (at < size && value[at] && channels < 16) {
                const size_t len = strnlen((const char *)value + at, size - at);
                snprintf(names[channels], sizeof names[channels], "%s", (const char *)value + at);
                at += len + 1;
                if (at + 16 > size)
                    break;
                memcpy(&types[channels], value + at, 4);
                at += 16;
                ++channels;
            }
        } else if (strcmp(name, "compression") == 0 && size >= 1) {
            compression = value[0];
        } else if (strcmp(name, "dataWindow") == 0 && size >= 16) {
            memcpy(window, value, 16);
        }
        free(value);
    }
    if (compression != 0) {
        fclose(f);
        snprintf(err, err_size, "compressed (%d): save it without compression", compression);
        return 0;
    }
    if (window[2] - window[0] + 1 != width || window[3] - window[1] + 1 != height) {
        fclose(f);
        snprintf(err, err_size, "%d x %d, not %d x %d", window[2] - window[0] + 1,
                 window[3] - window[1] + 1, width, height);
        return 0;
    }
    // Each channel's place in our pixels (-1: not used) and size in a line.
    int place[16];
    size_t line = 0;
    for (int c = 0; c < channels; ++c) {
        const char *n = names[c];
        place[c] = strcmp(n, "R") == 0   ? 0
                   : strcmp(n, "G") == 0 ? 1
                   : strcmp(n, "B") == 0 ? 2
                   : strcmp(n, "A") == 0 ? 3
                                         : -1;
        line += (size_t)width * (types[c] == 1 ? 2 : 4);
    }
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 0.0f;
        pixels[i * 4 + 3] = 1.0f;
    }
    if (fseek(f, 8L * height, SEEK_CUR) != 0) // the line offsets: the lines follow in order
        return fail(err, err_size, "truncated file", f);
    unsigned char *data = malloc(line);
    if (!data)
        return fail(err, err_size, "not enough memory", f);
    for (int y = 0; y < height; ++y) {
        unsigned char b[8];
        if (fread(b, 1, 8, f) != 8 || fread(data, 1, line, f) != line) {
            free(data);
            return fail(err, err_size, "truncated file", f);
        }
        const int32_t ly = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 |
                                     (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24) -
                           window[1];
        if (ly < 0 || ly >= height)
            continue;
        float *row = pixels + (size_t)(height - 1 - ly) * width * 4;
        const unsigned char *at = data;
        for (int c = 0; c < channels; ++c) {
            const size_t bytes = types[c] == 1 ? 2 : 4;
            for (int x = 0; place[c] >= 0 && x < width; ++x) {
                float v;
                if (types[c] == 1) {
                    uint16_t h;
                    memcpy(&h, at + (size_t)x * 2, 2);
                    v = from_half(h);
                } else if (types[c] == 2) {
                    memcpy(&v, at + (size_t)x * 4, 4);
                } else {
                    uint32_t u;
                    memcpy(&u, at + (size_t)x * 4, 4);
                    v = (float)u;
                }
                row[(size_t)x * 4 + place[c]] = v;
            }
            at += (size_t)width * bytes;
        }
    }
    free(data);
    fclose(f);
    return 1;
}
