#pragma once
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <memory>

// How the decoded samples should be interpreted on the way to the display.
// Filled from the AVFrame (falling back to the stream/codec parameters) and
// carried to the renderer, which applies the YUV matrix, the transfer function
// and the gamut conversion in the video pass.
struct ColorInfo {
    int matrix = 1;      // 0 = BT.601, 1 = BT.709, 2 = BT.2020 non-constant
    int primaries = 0;   // 0 = BT.709/sRGB, 1 = BT.2020
    int trc = 0;         // 0 = SDR (BT.709), 1 = PQ (SMPTE 2084), 2 = HLG
    bool fullRange = false;
    float peakNits = 100.f;   // mastering/content peak, used by the tone mapper

    bool hdr() const { return trc == 1 || trc == 2; }
};

// A decoded video frame plus its presentation timestamp (seconds).
// Holds a ref-counted AVFrame so the decoder can keep decoding ahead.
struct VideoFrame {
    AVFrame* frame = nullptr;   // owned
    double pts = 0.0;
    bool hw = false;            // true => AV_PIX_FMT_D3D11 (GPU texture array slice)
    AVPixelFormat format = AV_PIX_FMT_NONE;
    ColorInfo color;

    VideoFrame() = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    ~VideoFrame() {
        if (frame) av_frame_free(&frame);
    }
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;
