#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vTex;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D tex0;  // scene (HDR)
layout(set = 1, binding = 1) uniform sampler2D tex1;  // bloom
layout(set = 2, binding = 0) uniform sampler2D overlayTex;     // premultiplied sRGB RGBA (title, logo)
layout(set = 2, binding = 1) uniform sampler2D backgroundTex;  // sRGB background image
layout(push_constant) uniform PC { vec4 pc; } P;

vec3 aces(vec3 x) {  // Narkowicz ACES fit
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

void main() {
    vec2 uv = vTex;
    float glitch = F.fx2.y;
    if (glitch > 0.001) {
        float band = floor(vUv.y * 24.0);
        float slot = floor(timeS() * 12.0);
        float on = step(1.0 - glitch * 0.35, hash21(vec2(band, slot)));
        uv.x += on * (hash21(vec2(band + 7.0, slot)) - 0.5) * 0.08 * glitch;
    }
    vec2 d = uv - 0.5;
    float ca = F.fx.y * (1.0 + 2.0 * beatPulse());
    vec3 col;
    col.r = texture(tex0, uv + d * ca * 2.0).r;
    col.g = texture(tex0, uv).g;
    col.b = texture(tex0, uv - d * ca * 2.0).b;
    // The background image sits under the visuals (scenes are mostly dark, so it shows through).
    col += pow(texture(backgroundTex, uv).rgb, vec3(2.2)) * F.brand.y;
    col += texture(tex1, uv).rgb * F.fx.x * (1.0 + 0.5 * kickAmt());
    col *= F.fx2.z;
    col = aces(col);
    float vig = 1.0 - F.fx2.x * smoothstep(0.25, 0.95, length(d) * 1.35);
    col *= vig;
    col = srgb(col);
    vec4 ov = texture(overlayTex, vTex);
    col = col * (1.0 - ov.a * F.brand.x) + ov.rgb * F.brand.x;
    col += (hash21(vTex * F.res.xy + fract(timeS()) * 100.0) - 0.5) * F.fx.z;
    outColor = vec4(col, 1.0);
}
