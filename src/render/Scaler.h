#pragma once
#include <QString>

// Resampling filter used when the video size and the output rectangle differ.
// The values are what scale.frag expects in ubuf.extra.z, so keep them stable.
namespace Scaler {

enum Kind {
    Bilinear = 0,  // hardware bilinear in the present pass, no extra pass
    Bicubic = 1,   // Catmull-Rom, 2-texel support
    Lanczos3 = 2,  // windowed sinc, 3-texel support
};

// Kernel support in source texels (0 for the hardware sampler).
inline int support(int kind) { return kind == Bicubic ? 2 : (kind == Lanczos3 ? 3 : 0); }

// Case-insensitive. Anything unknown falls back to the default (bilinear), so a
// typo on the command line degrades instead of failing to render.
inline int fromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("bicubic") || n == QLatin1String("cubic") ||
        n == QLatin1String("catmull-rom")) {
        return Bicubic;
    }
    if (n == QLatin1String("lanczos3") || n == QLatin1String("lanczos") ||
        n == QLatin1String("lanczos-3")) {
        return Lanczos3;
    }
    return Bilinear;
}

inline const char* name(int kind) {
    switch (kind) {
    case Bicubic: return "bicubic";
    case Lanczos3: return "lanczos3";
    default: return "bilinear";
    }
}

}  // namespace Scaler
