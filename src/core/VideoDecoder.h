#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include "VideoFrame.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

// Video decoder: tries D3D11VA hardware decoding first, falls back to software.
// Hardware frames stay on the GPU (AV_PIX_FMT_D3D11); software frames go to the
// renderer as NV12/P010 planes, or as RGBA for formats the shader cannot read.
// The frame carries a ColorInfo so the renderer knows how to interpret them.
class VideoDecoder {
public:
    ~VideoDecoder();

    bool open(const AVStream* st, std::string* err);
    void close();

    bool sendPacket(const AVPacket* pkt);
    VideoFramePtr receiveFrame();
    void flush();

    bool isHardware() const { return hardware_; }
    AVPixelFormat swPixelFormat() const { return swFormat_; }
    AVBufferRef* hwDeviceCtx() const { return hwDeviceCtx_; }
    int width() const { return ctx_ ? ctx_->width : 0; }
    int height() const { return ctx_ ? ctx_->height : 0; }
    double frameDuration() const { return frameDuration_; }
    // Clockwise quarter turns needed before display (0..3), from the stream's
    // display matrix; |mirrored| is a horizontal flip in the same matrix.
    int rotation() const { return rotation_; }
    bool mirrored() const { return mirrored_; }

    // When on, hardware frames are handed to the renderer as-is (still in VRAM).
    // When off, they are pulled back to system memory as NV12.
    void setPassthrough(bool on) { passthrough_.store(on, std::memory_order_relaxed); }
    bool passthrough() const { return passthrough_.load(std::memory_order_relaxed); }

    ID3D11Device* d3dDevice() const;
    ID3D11DeviceContext* d3dContext() const;

    // FFmpeg's D3D11VA device context installs a recursive lock that serialises
    // all device-context/video-context calls. External code that touches the same
    // immediate context (e.g. the video processor) must use it too.
    bool d3dLock(void (**lock)(void*), void (**unlock)(void*), void** lockCtx) const;

private:
    static AVPixelFormat getFormatCB(AVCodecContext* ctx, const AVPixelFormat* fmts);
    bool openHardware(const AVStream* st);
    bool openSoftware(const AVStream* st);
    void readRotation(const AVStream* st);
    bool initHwFrames(AVPixelFormat swFmt);
    double framePts(const AVFrame* f) const;
    ColorInfo classify(const AVFrame* f) const;
    // Interlaced streams run through bwdif in the decoder: the fields are only
    // together in the decoded frame, and a progressive frame is what every
    // later stage (upload, scaling, subtitles) expects.
    AVFrame* deinterlace(AVFrame* in);
    bool buildVideoGraph(AVPixelFormat in);

    AVCodecContext* ctx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVRational timeBase_{1, 1000000};
    AVPixelFormat swFormat_ = AV_PIX_FMT_NONE;
    SwsContext* sws_ = nullptr;
    bool hardware_ = false;
    bool loggedHdr_ = false;
    int rotation_ = 0;      // clockwise quarter turns (0..3)
    bool mirrored_ = false;
    bool interlaced_ = false;
    bool deintDisabled_ = false;
    AVFieldOrder fieldOrder_ = AV_FIELD_UNKNOWN;
    AVFilterGraph* vgraph_ = nullptr;
    AVFilterContext* vbufSrc_ = nullptr;
    AVFilterContext* vbufSink_ = nullptr;
    AVPixelFormat vgraphIn_ = AV_PIX_FMT_NONE;
    std::atomic<bool> passthrough_{true};

    // Serialises every use of the shared D3D11 immediate context: FFmpeg's
    // decoder thread and our render thread (video processor) both use it, and
    // D3D11 contexts are not thread-safe. Recursive because FFmpeg's hwcontext
    // takes the same lock from inside avcodec_receive_frame()/transfer_data().
    std::recursive_mutex d3dMutex_;

    double lastPts_ = 0.0;
    double frameDuration_ = 1.0 / 30.0;
};
