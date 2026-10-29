// MS_Distort, Filter levels: `previous` (distort_atlas_x) reduced vertically by 1, 2,
// 4... stacked; level (nx, ny) sits at the sum of the sizes of the levels before it.
#version 410 core

uniform sampler2D previous;
uniform float source_height;
out vec4 color;

void main() {
    int height = int(source_height);
    ivec2 p = ivec2(gl_FragCoord.xy);
    int n = 0, y = p.y;
    while ((height >> n) > 0 && y >= (height >> n)) y -= height >> n++;
    if ((height >> n) == 0) { color = vec4(0.0); return; }
    int k = 1 << n;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < k; ++i) sum += texelFetch(previous, ivec2(p.x, y * k + i), 0);
    color = sum / float(k);
}
