// Horizontal box blur of `previous`: mean over [x - radius, x + radius], borders mirrored
// (pixel -1 = pixel 0). Three of these in a row make the effects' blur (common/blur.h).
#version 410 core

uniform sampler2D previous;
uniform float radius;
out vec4 color;

void main() {
    ivec2 size = textureSize(previous, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    int r = int(radius);
    vec4 sum = vec4(0.0);
    for (int i = -r; i <= r; ++i) {
        int x = p.x + i;
        x = x % (2 * size.x);  // mirrored, repeated for a box wider than the image
        if (x < 0) x += 2 * size.x;
        if (x >= size.x) x = 2 * size.x - 1 - x;
        sum += texelFetch(previous, ivec2(x, p.y), 0);
    }
    color = sum / float(2 * r + 1);
}
