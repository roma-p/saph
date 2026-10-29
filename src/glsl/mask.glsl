// The Mask input as one value (luma or alpha, maybe inverted), in every channel. See mask.h.
#version 410 core

uniform sampler2D mask;
uniform float mask_alpha;   // 1: use alpha, 0: use luma
uniform float invert_mask;
out vec4 color;

const vec3 LUMA = vec3(5.0, 8.0, 3.0) / 16.0;  // luma weights (common/mask.h)

void main() {
    vec4 m = texelFetch(mask, ivec2(gl_FragCoord.xy), 0);
    float v = mask_alpha > 0.5 ? m.a : dot(m.rgb, LUMA);
    color = vec4(invert_mask > 0.5 ? 1.0 - v : v);
}
