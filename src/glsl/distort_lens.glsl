// MS_Distort, lens: luma of the Lens (the Source when it is not connected), multiplied
// by the mask with Mask Lens. `previous` is the blurred mask value. Result in red.
#version 410 core

uniform sampler2D source;
uniform sampler2D lens;
uniform sampler2D previous;
uniform float has_lens, has_mask, mask_lens;
out vec4 color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 l = has_lens > 0.5 ? texelFetch(lens, p, 0) : texelFetch(source, p, 0);
    float value = (5.0 * l.r + 8.0 * l.g + 3.0 * l.b) / 16.0;
    if (has_mask > 0.5 && mask_lens > 0.5) value *= texelFetch(previous, p, 0).r;
    color = vec4(value, 0.0, 0.0, 0.0);
}
