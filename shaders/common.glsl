// Shared by every pass. Keep in sync with engine/include/av/render/frame_data.h (std140).
layout(std140, set = 0, binding = 0) uniform FrameBlock {
    vec4 res;       // x = width, y = height, z = aspect (w/h), w = time (s)
    vec4 audio;     // x = rms, y = kick, z = beat pulse, w = beat phase
    vec4 audio2;    // x = bar beat (0 = downbeat), y = section energy, z = bpm, w = hasTimeline
    vec4 fx;        // x = bloom, y = chroma, z = grain, w = feedback
    vec4 fx2;       // x = vignette, y = glitch, z = exposure, w = dt
    vec4 pal[4];    // palette
    vec4 bandsPk[2];
    vec4 barsPk[32];
    vec4 wavePk[64];
    vec4 ctl;       // x = transition, y = scene time, z = bar count
    vec4 brand;     // x = overlay opacity, y = background opacity
} F;

float barAt(int i) { return F.barsPk[i >> 2][i & 3]; }
float bandAt(int i) { return F.bandsPk[i >> 2][i & 3]; }
float waveAt(int i) { return F.wavePk[i >> 2][i & 3]; }
float timeS() { return F.res.w; }
float aspect() { return F.res.z; }
float beatPulse() { return F.audio.z; }
float kickAmt() { return F.audio.y; }
float rmsAmt() { return F.audio.x; }
bool isDownbeat() { return F.audio2.x < 0.5; }

vec3 hsv(float h, float s, float v) {
    vec3 k = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(vec3(1.0), k, s);
}

// Smooth palette lookup: t in [0,1) cycles through the four palette colours. Palette entries are
// authored as sRGB; scenes work in linear HDR, so this returns linear values.
vec3 palette(float t) {
    t = fract(t) * 4.0;
    int i = int(floor(t)) & 3;
    return pow(mix(F.pal[i].rgb, F.pal[(i + 1) & 3].rgb, smoothstep(0.0, 1.0, fract(t))), vec3(2.2));
}

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
