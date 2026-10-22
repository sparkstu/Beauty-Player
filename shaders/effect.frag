#version 440

// Colour-adjustment pass. It is deliberately trivial: it exists to prove the
// pass chain (video -> effect -> present) and to be the place where the beauty
// pipeline (skin smoothing, whitening, warp) will be inserted.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform buf {
    vec4 scale;
    vec4 params;
    vec4 extra;   // x = saturation, y = brightness, z/w = reserved
    vec4 color;
} ubuf;

void main() {
    vec3 c = texture(srcTex, v_uv).rgb;

    // Luma-preserving saturation around the Rec.709 grey point.
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, ubuf.extra.x);

    c += vec3(ubuf.extra.y);

    // In scRGB (color.w = 1) the values are linear light and may exceed 1.0;
    // clamping there would destroy every highlight.
    fragColor = vec4(ubuf.color.w > 0.5 ? max(c, vec3(0.0)) : clamp(c, 0.0, 1.0), 1.0);
}
