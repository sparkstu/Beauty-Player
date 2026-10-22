#pragma once
#include <cmath>
#include <cstdint>

// Display matrix (AV_PKT_DATA_DISPLAYMATRIX) -> sampling transform.
//
// Phone cameras store the sensor's landscape frame plus a 3x3 affine matrix that
// says how the frame has to be turned for display. FFmpeg writes it as
// fixed-point 16.16 with the convention  display = M * source, so the renderer
// has to sample  source = M^-1 * display.  Only the eight right-angle cases are
// supported (what cameras and muxers actually write): four rotations and the
// same four with a horizontal mirror.
namespace Rotation {

struct Info {
    int quarterTurns = 0;   // clockwise quarter turns applied while sampling
    bool mirrored = false;  // horizontal flip, applied before the rotation
};

// Mirrors av_display_rotation_get()'s angle convention but is independent of
// FFmpeg so the self-test can pin it down.
//
// A flipped matrix is "rotation * horizontal flip" (M = R * Fh). The flip is
// divided out before the angle is read; otherwise a pure horizontal flip would
// decompose into 180 degrees + flip, and "mirror then rotate 180" is a *vertical*
// flip, i.e. the opposite of what the matrix asked for.
inline Info fromMatrix(const int32_t m[9]) {
    Info info;
    const double a = double(m[0]);
    const double b = double(m[1]);
    const double d = double(m[3]);
    const double e = double(m[4]);
    const double det = a * e - b * d;
    if (det == 0.0) return info;  // degenerate: leave the frame as stored
    info.mirrored = det < 0.0;
    // Divide out Fh = diag(-1, 1): M * Fh negates the first column.
    const double ra = info.mirrored ? -a : a;
    const double rd = info.mirrored ? -d : d;
    constexpr double kPi = 3.14159265358979323846;
    // av_display_rotation_get() returns the counter-clockwise angle, so the
    // clockwise quarter turns are its negation.
    double cw = -std::atan2(rd, ra) * 180.0 / kPi;
    cw = std::fmod(cw, 360.0);
    if (cw < 0.0) cw += 360.0;
    const double quarters = std::round(cw / 90.0);
    if (std::fabs(cw - quarters * 90.0) > 2.0) return Info{};  // odd angle: ignore
    info.quarterTurns = int(quarters) % 4;
    return info;
}

}  // namespace Rotation
