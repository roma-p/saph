// MS_Test shader: output = source x gain (alpha unchanged).
#version 410 core

uniform sampler2D source;
uniform float gain;
out vec4 color;

void main() {
    vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    color = vec4(c.rgb * gain, c.a);
}
