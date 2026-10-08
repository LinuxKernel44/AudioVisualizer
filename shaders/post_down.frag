#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(push_constant) uniform PC { vec4 pc; } P;  // xy = source texel size

void main() {
    vec2 t = P.pc.xy;
    vec3 s = texture(tex0, vTex).rgb * 4.0;
    s += texture(tex0, vTex + t * vec2(-1, -1)).rgb;
    s += texture(tex0, vTex + t * vec2(1, -1)).rgb;
    s += texture(tex0, vTex + t * vec2(-1, 1)).rgb;
    s += texture(tex0, vTex + t * vec2(1, 1)).rgb;
    outColor = vec4(s * 0.125, 1.0);
}
