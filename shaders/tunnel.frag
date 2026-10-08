#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

// Kaleidoscopic tunnel: polar fold, perspective depth, glowing ring/spoke lattice.
void main() {
    float t = timeS();
    float pulse = beatPulse();
    float bpm = max(F.audio2.z, 90.0);
    vec2 p = (vUv - 0.5) * vec2(aspect(), 1.0) * 2.0;
    p += vec2(sin(t * 0.3), cos(t * 0.23)) * 0.12 * (0.5 + F.audio2.y);  // camera sway

    float r = max(length(p), 1e-3);
    float a = atan(p.y, p.x) + t * 0.1 + 0.15 * F.audio2.x;
    const float seg = 8.0;
    float k = PI / seg;
    a = abs(mod(a, 2.0 * k) - k);          // kaleidoscope fold

    float depth = 1.0 / r;
    // Forward speed locks to the tempo: one ring per beat at the current BPM.
    float z = depth * 0.8 + t * (bpm / 60.0) * 0.5 + pulse * 0.15;
    vec2 g = vec2(a / k, fract(z));

    float rings = exp(-abs(g.y - 0.5) * (14.0 - 6.0 * bandAt(1)));
    float spokes = exp(-abs(g.x - 0.5) * 18.0);
    float lattice = rings * (0.5 + bandAt(2)) + spokes * (0.35 + bandAt(4)) * 0.8;
    float cell = floor(z);
    float tint = hash11(cell * 1.7) * 0.5 + depth * 0.04 + t * 0.02;

    vec3 col = palette(tint).rgb * lattice * (0.9 + 1.4 * kickAmt() + 0.6 * pulse);
    col += palette(tint + 0.5).rgb * pow(rings * spokes, 1.5) * 2.2;     // bright nodes
    float fog = exp(-r * 0.15) * smoothstep(0.0, 0.6, r);               // dark centre, vignette outside
    col *= fog * (0.6 + 0.8 * smoothstep(0.0, 0.6, r));
    col += palette(0.3).rgb * exp(-r * 4.0) * (0.15 + 0.8 * bandAt(0)) * (0.6 + pulse);   // sub-bass core
    outColor = vec4(col, 1.0);
}
