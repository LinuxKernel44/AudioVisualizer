#version 450
layout(location = 0) out vec2 vUv;

void main() {
    // Single oversized triangle covering the screen; uv in [0,1] with y pointing up.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
