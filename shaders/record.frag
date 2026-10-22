#version 440

// Records the chain output into an RGBA8 texture the CPU can read back.
//
// The recorder runs as a branch off the end of the chain, so the preview keeps
// sampling the same texture and nothing about the display path changes.
//
//   SDR: the chain already holds sRGB-encoded bytes, this pass copies them.
//   HDR: the chain is linear scRGB (FP16, 1.0 = 80 nits). Tone map it back to
//        SDR with the same soft knee the SDR path uses, so a recording made on
//        an HDR display looks like the SDR presentation of the same frame.
//        (HDR files are a separate feature: this one writes BT.709 8-bit.)
//
//   extra.x = SDR white in scRGB units
//   extra.y = display peak in scRGB units
//   extra.z = 1 when the chain is linear scRGB

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform buf {
    vec4 scale;
    vec4 params;
    vec4 extra;
    vec4 color;
} ubuf;

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

vec3 softClip(vec3 x, float peak) {
    peak = max(peak, 1.0);
    const float knee = 0.75 * peak;
    const float range = peak - knee;
    vec3 over = max(x - knee, vec3(0.0));
    return min(x, vec3(knee)) + range * (vec3(1.0) - exp(-over / range));
}

void main() {
    vec3 c = texture(srcTex, v_uv).rgb;
    if (ubuf.extra.z > 0.5) {
        const float white = max(ubuf.extra.x, 1e-4);
        c = max(c, vec3(0.0)) / white;                     // 1.0 = SDR white
        c = softClip(c, max(ubuf.extra.y / white, 1.0));   // roll off highlights
        c = linearToSrgb(c);
    }
    fragColor = vec4(c, 1.0);
}
