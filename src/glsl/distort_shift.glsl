// MS_Distort, shift: slope of the (blurred) lens in `previous` (central difference,
// one-sided at the borders) times `amount`, per axis, turned by the warp direction and
// rounded to 1/256 pixel. Result: shift x in red, y in green.
#version 410 core

uniform sampler2D previous;
uniform float amount;   // pixels of shift per unit of lens slope, gain of the blur included
uniform float rel_x, rel_y, cos_r, sin_r;
out vec4 color;

float lens(ivec2 p) { return texelFetch(previous, p, 0).r; }

void main() {
    ivec2 size = textureSize(previous, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    int l = max(p.x - 1, 0), r = min(p.x + 1, size.x - 1);
    int b = max(p.y - 1, 0), t = min(p.y + 1, size.y - 1);
    float gx = r > l ? (lens(ivec2(r, p.y)) - lens(ivec2(l, p.y))) / float(r - l) : 0.0;
    float gy = t > b ? (lens(ivec2(p.x, t)) - lens(ivec2(p.x, b))) / float(t - b) : 0.0;
    float vx = amount * rel_x * gx, vy = amount * rel_y * gy;
    vec2 s = vec2(cos_r * vx - sin_r * vy, sin_r * vx + cos_r * vy);
    color = vec4(floor(s * 256.0 + 0.5) / 256.0, 0.0, 0.0);
}
