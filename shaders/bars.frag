#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform Frame {
    vec4 params;     // x = aspect (w/h), y = time (s), z = rms, w = bar count
    vec4 bars[32];   // up to 128 bar heights, packed 4 per vec4
} u;

vec3 hsv(float h, float s, float v) {
    vec3 k = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(vec3(1.0), k, s);
}

float barHeight(int i) { return u.bars[i >> 2][i & 3]; }

void main() {
    const float count = u.params.w;
    const float t = u.params.y;
    const float rms = u.params.z;

    // Background: deep gradient that breathes with the overall level.
    vec3 col = mix(vec3(0.01, 0.01, 0.03), vec3(0.05, 0.02, 0.10) + rms * 0.6, vUv.y);

    const float baseline = 0.30;                 // bars grow upward from here
    const float maxH = 0.55;
    float x = vUv.x * count;
    int idx = clamp(int(floor(x)), 0, int(count) - 1);
    float cell = fract(x);
    float h = barHeight(idx) * maxH;
    float hue = fract(0.62 + 0.55 * float(idx) / count + t * 0.02);

    float inBarX = smoothstep(0.08, 0.16, cell) * (1.0 - smoothstep(0.84, 0.92, cell));
    float y = vUv.y;

    // Main bar (above baseline) and its dim reflection (below).
    float up = y - baseline;
    if (up >= 0.0) {
        float inside = (1.0 - smoothstep(h - 0.004, h + 0.004, up)) * inBarX;
        vec3 c = hsv(hue, 0.75, 0.55 + 0.45 * (up / max(h, 0.001)));
        col = mix(col, c, inside);
        // Glow above the bar top.
        float glow = exp(-max(up - h, 0.0) * 40.0) * inBarX * clamp(h * 6.0, 0.0, 1.0);
        col += hsv(hue, 0.6, 1.0) * glow * 0.35;
    } else {
        float d = -up;
        float inside = (1.0 - smoothstep(h * 0.45 - 0.004, h * 0.45 + 0.004, d)) * inBarX;
        vec3 c = hsv(hue, 0.7, 0.35) * (1.0 - d / max(h * 0.45, 0.001));
        col = mix(col, c, inside * 0.55);
    }

    // Baseline hairline.
    col += vec3(0.4, 0.5, 0.9) * (1.0 - smoothstep(0.0, 0.002, abs(y - baseline))) * 0.5;
    outColor = vec4(col, 1.0);
}
