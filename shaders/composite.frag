#version 440

// Composites the subtitle overlay (premultiplied alpha) over the video. This is
// the last pass in the chain: the vertex stage applies the letterbox scale and
// both textures are sampled 1:1, because the overlay is rasterised at exactly
// the size of the content rectangle (no resampling of the text).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;
layout(binding = 2) uniform sampler2D overTex;

layout(std140, binding = 0) uniform buf {
    vec4 scale;
    vec4 params;
    vec4 extra;
    vec4 color;
} ubuf;

void main() {
    vec4 base = texture(srcTex, v_uv);
    // extra.x is 0 while no cue is on screen, so the overlay texture is not
    // touched between lines (no per-cue full-frame upload).
    if (ubuf.extra.x < 0.5) {
        fragColor = vec4(base.rgb, 1.0);
        return;
    }
    vec4 over = texture(overTex, v_uv);  // premultiplied
    // extra.z lifts the overlay to SDR white in linear scRGB output (1.0 in
    // SDR mode); text stays readable instead of sitting at 80 nits.
    const float white = ubuf.extra.z <= 0.0 ? 1.0 : ubuf.extra.z;
    fragColor = vec4(base.rgb * (1.0 - over.a) + over.rgb * white, 1.0);
}
