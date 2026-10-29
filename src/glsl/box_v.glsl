// Vertical box blur of `previous`: mean over [y - radius, y + radius], borders mirrored
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
        int y = p.y + i;
        y = y % (2 * size.y);  // mirrored, repeated for a box wider than the image
        if (y < 0) y += 2 * size.y;
        if (y >= size.y) y = 2 * size.y - 1 - y;
        sum += texelFetch(previous, ivec2(p.x, y), 0);
    }
    color = sum / float(2 * r + 1);
}
