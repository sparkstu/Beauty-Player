#version 440

// Resampling pass. Replaces the present pass when the user picked a better
// filter than the hardware bilinear sampler. The frame is already display-ready
// RGB at this point (video pass has done YUV/HDR), so this pass only samples it
// on the output grid.
//
//   extra.x = tap spacing along U, in source texels (1 = upscale, >1 = downscale)
//   extra.y = the same for V
//   extra.z = filter (1 = Catmull-Rom bicubic, 2 = Lanczos3)
//
// Taps sit on source texel centres. The kernel widens with the downscale ratio,
// so 4K -> 1080p is a real low-pass instead of point sampling, and big
// reductions are truncated (weights are normalised, so the image does not get
// brighter or darker when taps are dropped).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform buf {
    vec4 scale;
    vec4 params;
    vec4 extra;
    vec4 color;
} ubuf;

// 13 taps per axis (half width 6). The kernel half width is ceil(support * ratio)
// where ratio is the downscale factor, so this covers Lanczos3 (support 3) up to
// 2x and Catmull-Rom (support 2) up to 3x without truncating. Bigger reductions
// still truncate; the output grid is small by then, so a mip chain would cost
// more than it buys.
const int kMaxTaps = 13;

float catmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

float lanczos3(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    const float pi = 3.141592653589793;
    const float px = pi * x;
    return (sin(px) / px) * (sin(px / 3.0) / (px / 3.0));
}

float kernel(float x, int kind) {
    return kind == 2 ? lanczos3(x) : catmullRom(x);
}

// Number of taps needed to cover the (possibly widened) support.
int tapCount(float spacing, int kind) {
    const float support = kind == 2 ? 3.0 : 2.0;
    int halfWidth = int(ceil(support * spacing));
    halfWidth = clamp(halfWidth, 1, kMaxTaps / 2);
    return halfWidth * 2 + 1;
}

void main() {
    const vec2 texSize = vec2(textureSize(srcTex, 0));
    // Continuous source coordinate; taps are placed on texel centres around it.
    const vec2 coord = v_uv * texSize - 0.5;
    const vec2 base = floor(coord);
    const vec2 frac = coord - base;

    const int kind = int(ubuf.extra.z + 0.5);
    const vec2 spacing = max(ubuf.extra.xy, vec2(1.0));
    const int nx = tapCount(spacing.x, kind);
    const int ny = tapCount(spacing.y, kind);
    const int cx = (nx - 1) / 2;
    const int cy = (ny - 1) / 2;

    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int j = 0; j < kMaxTaps; ++j) {
        if (j > 2 * cy) break;
        const float dy = float(j - cy);
        // Distance from the sample point, in units of the widened kernel.
        const float wy = kernel((dy - frac.y) / spacing.y, kind) / spacing.y;
        if (wy == 0.0) continue;
        for (int i = 0; i < kMaxTaps; ++i) {
            if (i > 2 * cx) break;
            const float dx = float(i - cx);
            const float wx = kernel((dx - frac.x) / spacing.x, kind) / spacing.x;
            if (wx == 0.0) continue;
            const vec2 t = clamp(base + vec2(dx, dy), vec2(0.0), texSize - 1.0);
            acc += texture(srcTex, (t + 0.5) / texSize).rgb * (wx * wy);
            wsum += wx * wy;
        }
    }

    fragColor = vec4(wsum > 1e-6 ? acc / wsum : texture(srcTex, v_uv).rgb, 1.0);
}
