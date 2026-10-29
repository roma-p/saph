// MS_Distort, first half of the Filter levels: the Source reduced horizontally by 1, 2,
// 4... side by side (level n = mean of 2^n pixels, an odd last pixel dropped).
#version 410 core

uniform sampler2D source;
out vec4 color;

void main() {
    int width = textureSize(source, 0).x;
    ivec2 p = ivec2(gl_FragCoord.xy);
    int n = 0, x = p.x;
    while ((width >> n) > 0 && x >= (width >> n)) x -= width >> n++;
    if ((width >> n) == 0) { color = vec4(0.0); return; }
    int k = 1 << n;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < k; ++i) sum += texelFetch(source, ivec2(x * k + i, p.y), 0);
    color = sum / float(k);
}
