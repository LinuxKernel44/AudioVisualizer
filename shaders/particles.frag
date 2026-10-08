#version 450
layout(location = 0) in vec2 vQuad;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float d = dot(vQuad, vQuad);
    if (d > 1.0) discard;
    float a = (1.0 - d);
    outColor = vec4(vColor.rgb * a * a, 1.0);  // additive; soft round sprite
}
