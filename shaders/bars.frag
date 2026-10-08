#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    const float count = max(F.ctl.z, 1.0);
    const float pulse = beatPulse();
    const float secEnergy = F.audio2.y;
    const bool timeline = F.audio2.w > 0.5;
    const float baseHue = (timeline ? 0.15 * secEnergy : 0.0) + timeS() * 0.01;

    vec3 col = mix(vec3(0.0004, 0.0004, 0.0015), vec3(0.004, 0.0015, 0.012) + rmsAmt() * 0.02, vUv.y);
    col += palette(baseHue + 0.4).rgb * (pulse * (isDownbeat() ? 0.05 : 0.025) + kickAmt() * 0.04);

    const float baseline = 0.30;
    const float maxH = 0.55;
    float x = vUv.x * count;
    int idx = clamp(int(floor(x)), 0, int(count) - 1);
    float cell = fract(x);
    float h = clamp(barAt(idx) * (1.0 + 0.10 * pulse), 0.0, 1.0) * maxH;
    vec3 barCol = palette(baseHue + float(idx) / count * 0.75);

    float inBarX = smoothstep(0.08, 0.16, cell) * (1.0 - smoothstep(0.84, 0.92, cell));
    float up = vUv.y - baseline;
    if (up >= 0.0) {
        float inside = (1.0 - smoothstep(h - 0.004, h + 0.004, up)) * inBarX;
        col = mix(col, barCol * (0.45 + 0.55 * (up / max(h, 0.001))), inside);
        float glow = exp(-max(up - h, 0.0) * 40.0) * inBarX * clamp(h * 6.0, 0.0, 1.0);
        col += barCol * glow * (0.6 + 1.0 * pulse);  // HDR: feeds the bloom pass
    } else {
        float d = -up;
        float inside = (1.0 - smoothstep(h * 0.45 - 0.004, h * 0.45 + 0.004, d)) * inBarX;
        col = mix(col, barCol * 0.35 * (1.0 - d / max(h * 0.45, 0.001)), inside * 0.55);
    }
    float line = 1.0 - smoothstep(0.0, 0.002, abs(vUv.y - baseline));
    col += vec3(0.12, 0.2, 0.8) * line * (0.5 + (isDownbeat() ? 1.5 : 0.0) * pulse);
    outColor = vec4(col, 1.0);
}
