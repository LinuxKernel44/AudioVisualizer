#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// Animated Julia set with orbit traps. Cheap (64 iterations of z^2 + c per pixel) and rich:
// the parameter c orbits near the set boundary, the kick breathes the zoom, bands shift colours.
void main() {
    float t = timeS();
    float pulse = beatPulse();
    vec2 p = (vUv - 0.5) * vec2(aspect(), 1.0);
    float zoom = 2.3 - 0.35 * sin(t * 0.07) - 0.18 * kickAmt() - 0.1 * pulse;
    float ang = t * 0.05 + F.audio2.x * 0.05;
    mat2 R = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));
    vec2 z = R * p * zoom;

    float a = t * 0.11 + 0.9 * F.audio2.y;
    vec2 c = vec2(0.7885 * cos(a), 0.7885 * sin(a));
    c += 0.012 * vec2(bandAt(0) - 0.4, bandAt(3) - 0.4) + 0.02 * kickAmt();

    float trapLine = 1e5, trapPoint = 1e5;
    int it = 0;
    const int MAXI = 64;
    for (; it < MAXI; ++it) {
        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        trapLine = min(trapLine, abs(z.y * 0.8 + z.x * 0.2));
        trapPoint = min(trapPoint, length(z - vec2(0.3 * sin(t * 0.3), 0.3 * cos(t * 0.2))));
        if (dot(z, z) > 256.0) break;
    }
    vec3 col;
    if (it == MAXI) {
        // Interior: dark glass lit by the orbit traps.
        col = palette(0.55 + trapPoint * 0.4).rgb * exp(-trapPoint * 3.0) * 0.7
            + palette(0.1).rgb * exp(-trapLine * 8.0) * 0.5;
    } else {
        float sm = float(it) - log2(max(log2(dot(z, z)), 1e-4)) + 4.0;   // smooth escape time
        float k = sm * 0.045 + t * 0.03;
        col = palette(k).rgb * (0.04 + 0.45 * exp(-sm * 0.06));
        col += palette(k + 0.5).rgb * exp(-trapLine * 22.0) * (0.45 + 1.1 * pulse);   // filaments
        col += palette(k + 0.25).rgb * exp(-trapPoint * 9.0) * 0.35;
    }
    col *= 0.7 + 0.5 * rmsAmt() + 0.4 * kickAmt();
    col *= 1.0 - 0.45 * length(p);
    outColor = vec4(col, 1.0);
}
