#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const float TAU = 6.28318530718;

// Radial spectrum with a waveform ring. Bars are mirrored left/right so the picture is symmetric.
void main() {
    float t = timeS();
    float pulse = beatPulse();
    vec2 p = (vUv - vec2(0.5, 0.5)) * vec2(aspect(), 1.0);
    float r = length(p);
    float ang = atan(p.x, p.y);          // 0 at the top, clockwise
    float u = abs(ang) / 3.14159265;      // 0..1 mirrored around the vertical axis

    const float count = max(F.ctl.z, 1.0);
    float x = u * count;
    int idx = clamp(int(floor(x)), 0, int(count) - 1);
    float cell = fract(x);
    float level = barAt(idx);

    float base = 0.20 + 0.035 * kickAmt() + 0.02 * pulse;
    float len = level * 0.30;
    float inBar = smoothstep(0.10, 0.2, cell) * (1.0 - smoothstep(0.8, 0.9, cell));
    float outer = base + len;
    float body = smoothstep(base - 0.004, base, r) * (1.0 - smoothstep(outer - 0.004, outer, r)) * inBar;

    vec3 col = vec3(0.0005, 0.0005, 0.002) + palette(0.6).rgb * 0.004 / (r + 0.15);
    vec3 barCol = palette(u * 0.9 + t * 0.02);
    col += barCol * body * (0.6 + 0.9 * (r - base) / max(len, 0.01));
    col += barCol * exp(-max(r - outer, 0.0) * 28.0) * inBar * clamp(level * 3.0, 0.0, 1.0) * (0.5 + 0.6 * pulse);

    // Waveform ring just inside the bars.
    float wi = u * float(255);
    float w0 = waveAt(int(floor(wi)));
    float w1 = waveAt(min(int(floor(wi)) + 1, 255));
    float w = mix(w0, w1, fract(wi));
    float ringR = base - 0.045 + w * 0.05;
    float line = exp(-abs(r - ringR) * 220.0);
    col += palette(0.15 + t * 0.03).rgb * line * (1.2 + pulse);

    // Soft beams that sweep on beats.
    float beams = pow(max(0.0, sin(ang * 6.0 + t * 0.6 + F.audio2.x * 0.8)), 24.0);
    col += palette(0.4).rgb * beams * exp(-r * 3.0) * (0.06 + 0.2 * pulse);
    outColor = vec4(col, 1.0);
}
