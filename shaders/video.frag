#version 440

// Video / present pass.
//
//   params.x = 0  YUV planes (NV12 8-bit, P010 10-bit), converted here
//   params.x = 1  RGB already in the output encoding (effect/subtitle chain)
//   params.x = 2  RGB straight from a video frame (upload path)
//   params.x = 3  RGB display-referred sRGB (zero-copy / VideoProcessor output)
//   params.y = YUV matrix (0 = BT.601, 1 = BT.709, 2 = BT.2020 NCL)
//   params.z = 1 for full-range input
//   params.w = transfer function (0 = SDR, 1 = PQ, 2 = HLG)
//   extra.x  = SDR white in scRGB units (sdrWhiteNits / 80), HDR output only
//   extra.y  = display peak in scRGB units (peakNits / 80), HDR output only
//   color.x  = primaries (0 = BT.709/sRGB, 1 = BT.2020)
//   color.y  = content peak in nits (SDR tone-mapping roll-off)
//   color.z  = 1 to run the HDR path (transfer decode + tone map)
//   color.w  = 1 when the swapchain is scRGB: write linear, no sRGB encode
//
// SDR BT.709 into an SDR swapchain takes the old fast path: one matrix, no
// linearisation, exactly the same pixels as before this file grew a colour
// pipeline.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uvTex;

layout(std140, binding = 0) uniform buf {
    vec4 scale;
    vec4 params;
    vec4 extra;
    vec4 color;
} ubuf;

vec3 pqToLinear(vec3 e) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(max(e, vec3(0.0)), vec3(1.0 / m2));
    vec3 num = max(p - c1, vec3(0.0));
    vec3 den = max(c2 - c3 * p, vec3(1e-6));
    return pow(num / den, vec3(1.0 / m1)) * 10000.0;  // cd/m^2
}

float hlgInvOetf(float e) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    return e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

// HLG scene light + OOTF (system gamma 1.2) on a 1000-nit display.
vec3 hlgToLinear(vec3 e) {
    vec3 s = vec3(hlgInvOetf(e.r), hlgInvOetf(e.g), hlgInvOetf(e.b));
    float y = dot(s, vec3(0.2627, 0.6780, 0.0593));
    return s * pow(max(y, 1e-6), 0.2) * 1000.0;
}

vec3 srgbToLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

// ACES filmic approximation (Narkowicz). Normalised by the curve value at the
// content peak, so peak white lands exactly on 1.0.
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}

// BT.2020 -> BT.709 in linear light (columns, as GLSL expects).
vec3 bt2020ToBt709(vec3 c) {
    return mat3(1.6605, -0.1246, -0.0182,
                -0.5876, 1.1329, -0.0083,
                -0.0728, -0.1006, 1.1187) * c;
}

// C1 soft knee at 75% of the peak, asymptotic at the peak. Mirrors
// Hdr::softClip() so highlights above the panel's peak roll off instead of
// being hard clipped by the compositor.
vec3 softClip(vec3 x, float peak) {
    peak = max(peak, 1.0);
    const float knee = 0.75 * peak;
    const float range = peak - knee;
    vec3 over = max(x - knee, vec3(0.0));
    return min(x, vec3(knee)) + range * (vec3(1.0) - exp(-over / range));
}

void main() {
    const int mode = int(ubuf.params.x + 0.5);
    const bool hdrOut = ubuf.color.w > 0.5;
    vec3 rgb;

    if (mode == 0) {
        float y = texture(yTex, v_uv).r;
        vec2 c = texture(uvTex, v_uv).rg;
        if (ubuf.params.z > 0.5) {
            c -= vec2(0.5);           // full range
        } else {
            y = (y - 16.0 / 255.0) * (255.0 / 219.0);
            c = (c - vec2(0.5)) * (255.0 / 224.0);
        }
        const int m = int(ubuf.params.y + 0.5);
        if (m == 0) {                  // BT.601
            rgb = vec3(y + 1.40200 * c.y,
                       y - 0.34414 * c.x - 0.71414 * c.y,
                       y + 1.77200 * c.x);
        } else if (m == 2) {           // BT.2020 non-constant luminance
            rgb = vec3(y + 1.47460 * c.y,
                       y - 0.16455 * c.x - 0.57135 * c.y,
                       y + 1.88140 * c.x);
        } else {                       // BT.709
            rgb = vec3(y + 1.57480 * c.y,
                       y - 0.18733 * c.x - 0.46813 * c.y,
                       y + 1.85560 * c.x);
        }
    } else {
        rgb = texture(yTex, v_uv).rgb;
        if (mode == 1) {               // chain output: already target-encoded
            fragColor = vec4(rgb, 1.0);
            return;
        }
        if (mode == 3) {               // display-referred sRGB (zero-copy)
            fragColor = hdrOut ? vec4(srgbToLinear(rgb) * ubuf.extra.x, 1.0)
                               : vec4(rgb, 1.0);
            return;
        }
    }

    if (ubuf.color.z > 0.5) {
        // HDR source: encoded R'G'B' -> absolute nits, gamut to BT.709.
        const float t = ubuf.params.w;
        vec3 lin = (t > 1.5) ? hlgToLinear(rgb) : pqToLinear(rgb);
        if (ubuf.color.x > 0.5) lin = bt2020ToBt709(lin);
        lin = max(lin, vec3(0.0));
        if (hdrOut) {
            // scRGB: 1.0 = 80 nits, so absolute luminance divides by 80. The
            // soft knee sits at 75% of what the panel can show.
            rgb = softClip(lin / 80.0, ubuf.extra.y);
        } else {
            const float refWhite = 203.0;  // BT.2408 graphics white
            const float peak = max(ubuf.color.y, refWhite);
            const float w = acesFilm(vec3(peak / refWhite)).x;
            rgb = linearToSrgb(acesFilm(lin / refWhite) / max(w, 1e-4));
        }
    } else if (hdrOut || ubuf.color.x > 0.5) {
        // SDR source that needs linear light: scRGB output, or BT.2020 content
        // that has to be gamut-converted. Plain BT.709 into SDR keeps the old
        // byte-exact fast path (rgb is used as sampled).
        vec3 lin = srgbToLinear(rgb);
        if (ubuf.color.x > 0.5) lin = bt2020ToBt709(lin);
        lin = max(lin, vec3(0.0));
        if (hdrOut) {
            // Map SDR white to the user's SDR white level, like the rest of
            // the desktop, and keep the linear light for compositing.
            rgb = lin * ubuf.extra.x;
        } else {
            // SDR in BT.2020 primaries: gamut-convert without touching exposure.
            rgb = linearToSrgb(clamp(lin, 0.0, 1.0));
        }
    }

    fragColor = vec4(hdrOut ? max(rgb, vec3(0.0)) : clamp(rgb, 0.0, 1.0), 1.0);
}
