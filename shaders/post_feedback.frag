#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;  // current frame
layout(set = 1, binding = 1) uniform sampler2D tex1;  // previous result
layout(push_constant) uniform PC { vec4 pc; } P;      // x = feedback amount

void main() {
    vec2 c = vTex - 0.5;
    // Slow zoom + twist of the history gives trails that drift outwards on beats.
    float zoom = 1.0 - 0.006 - 0.012 * beatPulse();
    float ang = 0.002 + 0.004 * kickAmt();
    mat2 rot = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));
    vec2 uv = rot * c * zoom + 0.5;
    vec3 prev = texture(tex1, uv).rgb * 0.985;
    vec3 cur = texture(tex0, vTex).rgb;
    outColor = vec4(max(cur, prev * P.pc.x), 1.0);
}
