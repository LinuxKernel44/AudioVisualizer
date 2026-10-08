#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;  // outgoing scene
layout(set = 1, binding = 1) uniform sampler2D tex1;  // incoming scene
layout(push_constant) uniform PC { vec4 pc; } P;      // x = transition 0..1

// Scene transition: a quick radial wipe with a bright seam, so cuts land on the beat.
void main() {
    float t = P.pc.x;
    vec2 q = (vUv - 0.5) * vec2(aspect(), 1.0);
    float r = length(q) / (0.5 * length(vec2(aspect(), 1.0)));
    float edge = t * 1.3 - 0.15;
    float m = smoothstep(edge - 0.06, edge + 0.06, r);  // 0 inside (new scene), 1 outside (old)
    vec3 a = texture(tex0, vTex).rgb;
    vec3 b = texture(tex1, vTex).rgb;
    float seam = exp(-abs(r - edge) * 30.0) * sin(clamp(t, 0.0, 1.0) * 3.14159);
    outColor = vec4(mix(b, a, m) + palette(0.1).rgb * seam * 1.5, 1.0);
}
