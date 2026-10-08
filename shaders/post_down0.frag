#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(push_constant) uniform PC { vec4 pc; } P;  // xy = source texel size, z = threshold, w = knee

vec3 prefilter(vec3 c) {
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - P.pc.z + P.pc.w, 0.0, 2.0 * P.pc.w);
    soft = soft * soft / (4.0 * P.pc.w + 1e-4);
    float w = max(soft, br - P.pc.z) / max(br, 1e-4);
    return c * w;
}

void main() {
    vec2 t = P.pc.xy;
    // 13-tap downsample with Karis-style prefilter on the first level.
    vec3 a = prefilter(texture(tex0, vTex + t * vec2(-1, -1)).rgb);
    vec3 b = prefilter(texture(tex0, vTex + t * vec2(1, -1)).rgb);
    vec3 c = prefilter(texture(tex0, vTex + t * vec2(-1, 1)).rgb);
    vec3 d = prefilter(texture(tex0, vTex + t * vec2(1, 1)).rgb);
    vec3 e = prefilter(texture(tex0, vTex).rgb);
    outColor = vec4((a + b + c + d) * 0.125 + e * 0.5, 1.0);
}
