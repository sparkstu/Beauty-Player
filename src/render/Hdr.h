#pragma once
#include <algorithm>
#include <cmath>

// scRGB output helpers. Windows defines an FP16 scRGB (extended linear sRGB)
// swapchain component of 1.0 as 80 nits of luminance, while SDR white must be
// shown at the user's configured "SDR content brightness"
// (AdvancedColorInfo::SdrWhiteLevelInNits, exposed by QRhiSwapChainHdrInfo).
//
// These are the CPU-side mirrors of the constants in shaders/video.frag; the
// self-test pins them down so a refactor cannot silently change the exposure.
namespace Hdr {

constexpr float kScRgbWhiteNits = 80.f;        // scRGB component that is 1.0
constexpr float kDefaultSdrWhiteNits = 203.f;  // BT.2408 graphics white
constexpr float kDefaultPeakNits = 1000.f;

// sRGB 1.0 (SDR white) as a linear scRGB component.
inline float sdrWhiteScale(float sdrWhiteNits) {
    const float nits = sdrWhiteNits > 1.f ? sdrWhiteNits : kDefaultSdrWhiteNits;
    return nits / kScRgbWhiteNits;
}

// Absolute luminance (cd/m^2) as a linear scRGB component.
inline float nitsToScRgb(float nits) { return nits / kScRgbWhiteNits; }

// Soft knee starting at 75% of |peak|, asymptotic at |peak|: C1 continuous, so
// highlights above the display peak roll off instead of hard clipping.
inline float softClip(float x, float peak) {
    if (peak <= 0.f) return 0.f;
    if (x <= 0.f) return 0.f;
    const float knee = 0.75f * peak;
    if (x <= knee) return x;
    const float range = peak - knee;
    return knee + range * (1.f - std::exp(-(x - knee) / range));
}

}  // namespace Hdr
