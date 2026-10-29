// MS_Distort, warp: output pixel p = Source at p + shift (pass15), bilinear, with the
// Wrap modes inside the crop. With Filter, it reads the reduced copies (pass17) where
// the warp shrinks the image, blending two levels (see ms_distort.c). Then the mask
// (pass7): mixed after the warp (Post-effect) or moved with it (Pre-effect).
#version 410 core

uniform sampler2D source;
uniform sampler2D pass7;    // blurred mask value
uniform sampler2D pass15;   // shift
uniform sampler2D pass17;   // levels
uniform float wrap_x, wrap_y;                      // 0 No, 1 Tile, 2 Reflect
uniform float crop_l, crop_r, crop_b, crop_t;      // pixels
uniform float filter_on, levels_x, levels_y;
uniform float has_mask, pre_effect;
out vec4 color;

ivec2 size;

// Brings s inside [lo, hi) with the wrap mode; false if nothing can be read (No).
bool wrap(inout float s, float lo, float hi, int mode) {
    float n = hi - lo;
    if (n <= 0.0) return false;
    if (mode == 0) return s > lo - 1.0 && s < hi;
    float v;
    if (mode == 1) {
        v = lo + mod(s - lo, n);
    } else {
        v = lo + mod(s - lo, 2.0 * n);
        if (v > hi) v = 2.0 * hi - v;
        if (v > hi - 1.0) v = hi - 1.0;
    }
    s = v;
    return true;
}

// Pixel read for index i of a level n pixels long whose pixels cover `scale` Source
// pixels; -1 for transparent (Wrap No).
int tap(int i, int n, int scale, float lo_s, float hi_s, int mode) {
    int lo = int(ceil(lo_s / float(scale))), hi = min(int(floor(hi_s / float(scale))), n);
    if (hi <= lo) hi = lo + 1;
    if (i >= lo && i < hi) return i;
    if (mode == 0) return -1;
    if (mode == 1) return i < lo ? i + (hi - lo) : i - (hi - lo);
    return i < lo ? lo : hi - 1;
}

int offset(int length, int n) {  // where level n starts along an axis
    int o = 0;
    for (int k = 0; k < n; ++k) o += length >> k;
    return o;
}

// Bilinear sample of level (nx, ny) at Source position s.
vec4 level(int nx, int ny, vec2 s) {
    int w = size.x >> nx, h = size.y >> ny;
    ivec2 o = ivec2(offset(size.x, nx), offset(size.y, ny));
    vec2 q = (s + 0.5) / vec2(1 << nx, 1 << ny) - 0.5;
    vec2 f0 = floor(q), f = q - f0;
    vec4 sum = vec4(0.0);
    for (int j = 0; j < 2; ++j) {
        float wy = j == 0 ? 1.0 - f.y : f.y;
        int ty = tap(int(f0.y) + j, h, 1 << ny, crop_b, float(size.y) - crop_t, int(wrap_y));
        if (ty < 0 || wy == 0.0) continue;
        for (int i = 0; i < 2; ++i) {
            float wx = i == 0 ? 1.0 - f.x : f.x;
            int tx = tap(int(f0.x) + i, w, 1 << nx, crop_l, float(size.x) - crop_r, int(wrap_x));
            if (tx < 0 || wx == 0.0) continue;
            sum += wx * wy * texelFetch(pass17, o + ivec2(tx, ty), 0);
        }
    }
    return sum;
}

// Level of detail for a footprint of d pixels: log2, linear between powers of two.
float fast_log2(float d) {
    if (d <= 1.0) return 0.0;
    float e = floor(log2(d));
    return e + d / exp2(e) - 1.0;
}

float footprint(vec2 d) {
    d = abs(d);
    return max(d.x, d.y) + 0.5 * min(d.x, d.y);
}

vec2 shift(int x, int y) { return texelFetch(pass15, ivec2(x, y), 0).xy; }

float mask_value(ivec2 p) { return texelFetch(pass7, p, 0).r; }

void main() {
    size = textureSize(source, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 sh = shift(p.x, p.y);
    vec2 s = vec2(p) + sh;
    vec4 rgba = vec4(0.0);
    bool inside_x = wrap(s.x, crop_l, float(size.x) - crop_r, int(wrap_x));
    bool inside_y = wrap(s.y, crop_b, float(size.y) - crop_t, int(wrap_y));
    if (inside_x && inside_y) {
        if (filter_on < 0.5) {
            rgba = level(0, 0, s);
        } else {
            // Derivatives of the shift (central differences, one-sided at the borders).
            int l = max(p.x - 1, 0), r = min(p.x + 1, size.x - 1);
            int b = max(p.y - 1, 0), t = min(p.y + 1, size.y - 1);
            vec2 dx = (shift(r, p.y) - shift(l, p.y)) / float(max(r - l, 1));
            vec2 dy = (shift(p.x, t) - shift(p.x, b)) / float(max(t - b, 1));
            float lx = min(fast_log2(footprint(dx)), levels_x - 1.0);
            float ly = min(fast_log2(footprint(dy)), levels_y - 1.0);
            int nx0 = int(floor(lx)), ny0 = int(floor(ly)), nx1 = int(ceil(lx)), ny1 = int(ceil(ly));
            float f = lx >= ly ? lx - float(nx0) : ly - float(ny0);
            rgba = (1.0 - f) * level(nx0, ny0, s);
            if (f > 0.0) rgba += f * level(nx1, ny1, s);
        }
    }
    vec4 src = texelFetch(source, p, 0);
    float m = 1.0;
    if (has_mask > 0.5 && pre_effect > 0.5) {
        // The mask moves with the image: read it at the shifted position.
        vec2 q = vec2(p) + sh;
        m = 0.0;
        if (wrap(q.x, 0.0, float(size.x), wrap_x == 1.0 ? 1 : 2) && wrap(q.y, 0.0, float(size.y), wrap_y == 1.0 ? 1 : 2)) {
            ivec2 a = ivec2(floor(q));
            ivec2 c = ivec2(a.x + 1 < size.x ? a.x + 1 : a.x, a.y + 1 < size.y ? a.y + 1 : a.y);
            vec2 f = q - vec2(a);
            m = mix(mix(mask_value(a), mask_value(ivec2(c.x, a.y)), f.x),
                    mix(mask_value(ivec2(a.x, c.y)), mask_value(c), f.x), f.y);
        }
    } else if (has_mask > 0.5) {
        m = mask_value(p);
    }
    color = src + (rgba - src) * m;
}
