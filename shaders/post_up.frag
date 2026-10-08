#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(push_constant) uniform PC { vec4 pc; } P;  // xy = source texel size, z = weight

void main() {
    vec2 t = P.pc.xy;
    vec3 s = texture(tex0, vTex + t * vec2(-2, 0)).rgb;
    s += texture(tex0, vTex + t * vec2(2, 0)).rgb;
    s += texture(tex0, vTex + t * vec2(0, -2)).rgb;
    s += texture(tex0, vTex + t * vec2(0, 2)).rgb;
    s += 2.0 * texture(tex0, vTex + t * vec2(-1, -1)).rgb;
    s += 2.0 * texture(tex0, vTex + t * vec2(1, -1)).rgb;
    s += 2.0 * texture(tex0, vTex + t * vec2(-1, 1)).rgb;
    s += 2.0 * texture(tex0, vTex + t * vec2(1, 1)).rgb;
    outColor = vec4(s / 12.0 * P.pc.z, 1.0);  // blended additively onto the next level
}
