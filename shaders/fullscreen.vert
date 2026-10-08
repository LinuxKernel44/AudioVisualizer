#version 450
layout(location = 0) out vec2 vUv;   // y up, 0..1
layout(location = 1) out vec2 vTex;  // texture coordinates (y down)

void main() {
    // One oversized triangle covering the screen.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vTex = p;
    vUv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
