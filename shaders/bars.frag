#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform Frame {
    vec4 params;     // x = aspect (w/h), y = time (s), z = rms, w = bar count
    vec4 bars[32];   // up to 128 bar heights, packed 4 per vec4
    vec4 beat;       // x = beat pulse, y = beat phase, z = bar beat (0 = downbeat), w = section energy
    vec4 misc;       // x = kick, y = bpm, z = hasTimeline
    vec4 bands[2];   // 7 normalised band energies
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
    const float pulse = u.beat.x;
    const bool downbeat = u.beat.z < 0.5;
    const float secEnergy = u.beat.w;
    const float kick = u.misc.x;
    const bool timeline = u.misc.z > 0.5;

    // The palette drifts slowly and shifts with the current section's energy.
    float baseHue = 0.62 + (timeline ? 0.18 * secEnergy : 0.0) + t * 0.01;

    // Background: deep gradient that breathes with the level, flashes on beats and kicks.
    vec3 bgTop = vec3(0.05, 0.02, 0.10) + rms * 0.6;
    vec3 bg = mix(vec3(0.01, 0.01, 0.03), bgTop, vUv.y);
    vec3 flash = hsv(baseHue + 0.1, 0.5, 1.0) * (pulse * (downbeat ? 0.16 : 0.08) + kick * 0.10);
    float vignette = 1.0 - 0.55 * smoothstep(0.35, 1.1, length(vUv - vec2(0.5, 0.45)) * 1.3);
    vec3 col = (bg + flash * (1.0 - vUv.y * 0.6)) * vignette;

    const float baseline = 0.30;
    const float maxH = 0.55;
    float x = vUv.x * count;
    int idx = clamp(int(floor(x)), 0, int(count) - 1);
    float cell = fract(x);
    float h = clamp(barHeight(idx) * (1.0 + 0.10 * pulse), 0.0, 1.0) * maxH;
    float hue = fract(baseHue + 0.55 * float(idx) / count);

    float inBarX = smoothstep(0.08, 0.16, cell) * (1.0 - smoothstep(0.84, 0.92, cell));
    float y = vUv.y;
    float up = y - baseline;
    if (up >= 0.0) {
        float inside = (1.0 - smoothstep(h - 0.004, h + 0.004, up)) * inBarX;
        vec3 c = hsv(hue, 0.75, 0.55 + 0.45 * (up / max(h, 0.001)));
        col = mix(col, c, inside);
        float glow = exp(-max(up - h, 0.0) * 40.0) * inBarX * clamp(h * 6.0, 0.0, 1.0);
        col += hsv(hue, 0.6, 1.0) * glow * (0.35 + 0.35 * pulse);
    } else {
        float d = -up;
        float inside = (1.0 - smoothstep(h * 0.45 - 0.004, h * 0.45 + 0.004, d)) * inBarX;
        vec3 c = hsv(hue, 0.7, 0.35) * (1.0 - d / max(h * 0.45, 0.001));
        col = mix(col, c, inside * 0.55);
    }

    // Baseline hairline, brighter on the downbeat.
    float line = 1.0 - smoothstep(0.0, 0.002, abs(y - baseline));
    col += vec3(0.4, 0.5, 0.9) * line * (0.5 + (downbeat ? 0.5 : 0.0) * pulse);
    outColor = vec4(col, 1.0);
}
