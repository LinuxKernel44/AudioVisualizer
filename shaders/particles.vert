#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

struct Particle { vec4 a; vec4 b; };
layout(set = 1, binding = 0, std430) readonly buffer Particles { Particle p[]; } buf;

layout(location = 0) out vec2 vQuad;
layout(location = 1) out vec4 vColor;

void main() {
    // Two triangles per particle: corner index from gl_VertexIndex (0..5), particle = gl_InstanceIndex.
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    vec2 c = corners[gl_VertexIndex];
    Particle q = buf.p[gl_InstanceIndex];

    // Orbiting camera.
    float t = timeS() * 0.12 + F.audio2.x * 0.0;
    float ca = cos(t), sa = sin(t);
    vec3 p = q.a.xyz;
    p = vec3(ca * p.x + sa * p.z, p.y, -sa * p.x + ca * p.z);
    float tilt = 0.35 + 0.1 * sin(timeS() * 0.07);
    p = vec3(p.x, cos(tilt) * p.y - sin(tilt) * p.z, sin(tilt) * p.y + cos(tilt) * p.z);
    p.z += 3.4 - 0.25 * beatPulse();

    float depth = max(p.z, 0.1);
    vec2 ndc = vec2(p.x / depth * 1.25, p.y / depth * 1.25 * aspect());
    float speed = length(q.b.xyz);
    float size = (0.0022 + 0.0022 * kickAmt()) / depth * (0.6 + q.b.w) * smoothstep(0.0, 0.1, q.a.w);
    ndc += c * vec2(size / aspect(), size) * 4.0 * vec2(1.0, aspect());
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);

    vec3 col = palette(q.b.w * 0.5 + speed * 0.12 + timeS() * 0.02).rgb;
    float fade = smoothstep(0.0, 0.2, q.a.w) * (0.35 + 0.65 * q.a.w);
    float nearFade = smoothstep(0.5, 1.6, depth);   // never draw huge blobs right in front of the camera
    vColor = vec4(col * (0.08 + 0.55 * smoothstep(0.0, 2.5, speed) + 0.25 * beatPulse()) * fade * nearFade, 1.0);
    vQuad = c;
}
