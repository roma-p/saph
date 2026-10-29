// MS_BandPass final pass: difference of the two blurs, saturation, brightness, offset
// darks, then mixed over the Source through the mask.
#version 410 core

uniform sampler2D source;
uniform sampler2D pass6;    // Blur Amount1 (passes 1-6)
uniform sampler2D pass12;   // Blur Amount2 (passes 7-12)
uniform sampler2D pass19;   // blurred mask (passes 13-19)
uniform float brightness;
uniform float saturation;
uniform float offset_darks;
uniform float has_mask;
out vec4 color;

const vec3 LUMA = vec3(5.0, 8.0, 3.0) / 16.0;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 src = texelFetch(source, p, 0);
    vec3 d = texelFetch(pass6, p, 0).rgb - texelFetch(pass12, p, 0).rgb;
    float l = dot(d, LUMA);
    d = l + saturation * (d - l);
    vec3 effect = 2.0 * brightness * d + offset_darks * (1.0 - 2.0 * d);
    float m = has_mask > 0.5 ? texelFetch(pass19, p, 0).r : 1.0;
    color = vec4(mix(src.rgb, effect, m), src.a);  // alpha unchanged
}
