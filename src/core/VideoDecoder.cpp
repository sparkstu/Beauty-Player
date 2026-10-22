// d3d11.h must not be pulled in from inside an extern "C" block, and
// hwcontext_d3d11va.h includes it -- so the Windows headers go first.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#endif

#include "VideoDecoder.h"
#include "Log.h"
#include "Rotation.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#ifdef _WIN32
namespace {

// C callbacks handed to FFmpeg's D3D11 device context (see
// AVD3D11VADeviceContext::lock). lock_ctx is a VideoDecoder's d3dMutex_.
void d3dLockCB(void* p) { static_cast<std::recursive_mutex*>(p)->lock(); }
void d3dUnlockCB(void* p) { static_cast<std::recursive_mutex*>(p)->unlock(); }

}  // namespace
#endif

namespace {

// Map FFmpeg's enums onto the small integer ids the shader understands. -1
// means "not signalled", so the caller can fall back to a heuristic.
int matrixId(AVColorSpace cs) {
    switch (cs) {
        case AVCOL_SPC_BT709: return 1;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return 2;
        case AVCOL_SPC_RGB: return 1;
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE240M:
        case AVCOL_SPC_FCC: return 0;
        default: return -1;
    }
}

int primariesId(AVColorPrimaries p) {
    switch (p) {
        case AVCOL_PRI_BT2020: return 1;
        case AVCOL_PRI_BT709: return 0;
        case AVCOL_PRI_BT470M:
        case AVCOL_PRI_BT470BG:
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M: return 0;
        default: return -1;
    }
}

int trcId(AVColorTransferCharacteristic t) {
    switch (t) {
        case AVCOL_TRC_SMPTE2084: return 1;      // PQ (HDR10 / Dolby Vision base)
        case AVCOL_TRC_ARIB_STD_B67: return 2;   // HLG
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_GAMMA22:
        case AVCOL_TRC_GAMMA28:
        case AVCOL_TRC_SMPTE240M: return 0;
        default: return -1;
    }
}

const char* matrixName(int m) {
    return m == 2 ? "bt2020nc" : (m == 0 ? "bt601" : "bt709");
}

const char* trcName(int t) {
    return t == 1 ? "pq" : (t == 2 ? "hlg" : "sdr");
}

}  // namespace

ColorInfo VideoDecoder::classify(const AVFrame* f) const {
    ColorInfo c;

    // Frame fields win (they follow per-frame metadata and container changes),
    // then the stream/codec parameters, then a resolution heuristic: SD is
    // BT.601, everything else BT.709.
    int m = matrixId(f ? static_cast<AVColorSpace>(f->colorspace) : AVCOL_SPC_UNSPECIFIED);
    if (m < 0 && ctx_) m = matrixId(ctx_->colorspace);
    const int w = f && f->width > 0 ? f->width : (ctx_ ? ctx_->width : 0);
    const int h = f && f->height > 0 ? f->height : (ctx_ ? ctx_->height : 0);
    c.matrix = m >= 0 ? m : (h >= 720 ? 1 : 0);

    int p = primariesId(f ? static_cast<AVColorPrimaries>(f->color_primaries)
                          : AVCOL_PRI_UNSPECIFIED);
    if (p < 0 && ctx_) p = primariesId(ctx_->color_primaries);
    c.primaries = p >= 0 ? p : (c.matrix == 2 ? 1 : 0);

    int t = trcId(f ? static_cast<AVColorTransferCharacteristic>(f->color_trc)
                    : AVCOL_TRC_UNSPECIFIED);
    if (t < 0 && ctx_) t = trcId(ctx_->color_trc);
    c.trc = t >= 0 ? t : 0;

    c.fullRange = (f && f->color_range == AVCOL_RANGE_JPEG) ||
                  (!f && ctx_ && ctx_->color_range == AVCOL_RANGE_JPEG) ||
                  (f && f->color_range == AVCOL_RANGE_UNSPECIFIED && ctx_ &&
                   ctx_->color_range == AVCOL_RANGE_JPEG);

    // HDR mastering metadata, when the container carries it, gives the peak
    // the tone mapper should roll off at.
    float peak = c.hdr() ? 1000.f : 100.f;
    if (f) {
        if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
            const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
            if (md && md->has_luminance && md->max_luminance.den > 0) {
                const double nits = av_q2d(md->max_luminance) / 10000.0;  // units of 0.0001 cd/m^2
                if (nits > 10.0) peak = float(nits);
            }
        }
        if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
            const auto* cll = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
            if (cll && cll->MaxCLL > 10 && float(cll->MaxCLL) < peak)
                peak = float(cll->MaxCLL);
        }
    }
    c.peakNits = peak;
    return c;
}


VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (vgraph_) {
        avfilter_graph_free(&vgraph_);
        vbufSrc_ = nullptr;
        vbufSink_ = nullptr;
        vgraphIn_ = AV_PIX_FMT_NONE;
    }
    if (ctx_) avcodec_free_context(&ctx_);
    if (hwDeviceCtx_) av_buffer_unref(&hwDeviceCtx_);
    hardware_ = false;
    loggedHdr_ = false;
    interlaced_ = false;
    fieldOrder_ = AV_FIELD_UNKNOWN;
}

AVPixelFormat VideoDecoder::getFormatCB(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    // Hardware not offered for this stream: let the decoder pick its native
    // software format, receiveFrame() will convert it to RGBA.
    return fmts[0];
}

// Phone cameras store the sensor's landscape frame plus a display matrix; without
// honouring it a portrait video plays sideways. Only multiples of 90 degrees are
// supported (what every camera writes) and the rotation is snapped to the nearest
// quarter turn; a non-zero determinant means the matrix also mirrors the image.
void VideoDecoder::readRotation(const AVStream* st) {
    rotation_ = 0;
    mirrored_ = false;
    if (!st) return;
    const AVPacketSideData* sd =
        av_packet_side_data_get(st->codecpar->coded_side_data,
                                st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * int(sizeof(int32_t))) return;
    int32_t m[9];
    std::memcpy(m, sd->data, sizeof(m));
    const Rotation::Info info = Rotation::fromMatrix(m);
    rotation_ = info.quarterTurns;
    mirrored_ = info.mirrored;
    if (rotation_ != 0)
        LOGI("rotation: %d degrees clockwise%s", rotation_ * 90, mirrored_ ? ", mirrored" : "");
    else if (mirrored_)
        LOGI("rotation: mirrored");
    else if (m[0] != 0 || m[1] != 0 || m[3] != 0 || m[4] != 0)
        LOGW("rotation: display matrix is not a right angle, ignoring");
}

bool VideoDecoder::open(const AVStream* st, std::string* err) {
    timeBase_ = st->time_base;
    readRotation(st);
    fieldOrder_ = st->codecpar->field_order;
    interlaced_ = fieldOrder_ == AV_FIELD_TT || fieldOrder_ == AV_FIELD_BB ||
                  fieldOrder_ == AV_FIELD_TB || fieldOrder_ == AV_FIELD_BT;
    deintDisabled_ = std::getenv("PLAY_NO_DEINT") != nullptr;
    if (interlaced_ && deintDisabled_) {
        // Escape hatch for A/B checks and for files that are flagged interlaced
        // but actually progressive.
        LOGW("video: interlaced source, deinterlacing disabled by PLAY_NO_DEINT");
        interlaced_ = false;
    }
    if (interlaced_)
        LOGI("video: interlaced source (%s), deinterlacing to progressive",
             fieldOrder_ == AV_FIELD_TT || fieldOrder_ == AV_FIELD_TB ? "top field first"
                                                                     : "bottom field first");
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        frameDuration_ = av_q2d(av_inv_q(st->avg_frame_rate));
    else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
        frameDuration_ = av_q2d(av_inv_q(st->r_frame_rate));

    if (openHardware(st)) {
        hardware_ = true;
        LOGI("video: hardware decoding (D3D11VA), %dx%d", ctx_->width, ctx_->height);
        return true;
    }
    LOGW("video: hardware decoding unavailable, using software");
    return openSoftware(st);
}

bool VideoDecoder::initHwFrames(AVPixelFormat swFmt) {
#ifdef _WIN32
    AVBufferRef* framesRef = av_hwframe_ctx_alloc(hwDeviceCtx_);
    if (!framesRef) return false;

    auto* fc = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = swFmt;
    fc->width = ctx_->width;
    fc->height = ctx_->height;

    // The decoder pool must be usable as a shader resource, otherwise the
    // decoded texture cannot be sampled by the renderer.
    auto* d3d = reinterpret_cast<AVD3D11VAFramesContext*>(fc->hwctx);
    d3d->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    if (av_hwframe_ctx_init(framesRef) < 0) {
        av_buffer_unref(&framesRef);
        return false;
    }
    ctx_->hw_frames_ctx = framesRef;
    return true;
#else
    (void)swFmt;
    return false;
#endif
}

bool VideoDecoder::openHardware(const AVStream* st) {
#ifdef _WIN32
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    avcodec_parameters_to_context(ctx_, st->codecpar);
    ctx_->pkt_timebase = st->time_base;

    // The decoder owns its device: the scene graph's device is created without
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT, which the video processor needs. The
    // two devices exchange the converted frame through a shared texture.
    if (av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) {
        LOGE("video: cannot create D3D11VA device");
        avcodec_free_context(&ctx_);
        return false;
    }

    if (auto* hwdc0 = reinterpret_cast<AVHWDeviceContext*>(hwDeviceCtx_->data)) {
        if (auto* d3d0 = reinterpret_cast<AVD3D11VADeviceContext*>(hwdc0->hwctx))
            LOGI("video: ffmpeg d3d11 device=%p context=%p", (void*)d3d0->device,
                 (void*)d3d0->device_context);
    }

    // Replace FFmpeg's internal device-context lock with ours, so that every
    // D3D11 immediate-context user (decode thread, hwframe transfer, and the
    // render thread's video processor) shares one recursive mutex.
    if (auto* hwdc = reinterpret_cast<AVHWDeviceContext*>(hwDeviceCtx_->data)) {
        if (auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(hwdc->hwctx)) {
            d3d->lock = &d3dLockCB;
            d3d->unlock = &d3dUnlockCB;
            d3d->lock_ctx = &d3dMutex_;
        }
    }

    ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    ctx_->opaque = this;
    ctx_->get_format = &VideoDecoder::getFormatCB;

    // 10-bit sources decode to P010, everything else to NV12.
    const bool tenBit = ctx_->pix_fmt == AV_PIX_FMT_YUV420P10LE ||
                        ctx_->pix_fmt == AV_PIX_FMT_YUV420P10BE ||
                        ctx_->pix_fmt == AV_PIX_FMT_P010LE ||
                        ctx_->pix_fmt == AV_PIX_FMT_P010BE;

    AVPixelFormat chosen = AV_PIX_FMT_NONE;
    if (initHwFrames(tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12)) {
        chosen = tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    } else if (initHwFrames(AV_PIX_FMT_NV12)) {
        chosen = AV_PIX_FMT_NV12;
    } else {
        LOGW("video: cannot init D3D11 hw frames context");
    }

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        av_buffer_unref(&hwDeviceCtx_);
        return false;
    }
    swFormat_ = chosen;
    return true;
#else
    (void)st;
    return false;
#endif
}

bool VideoDecoder::openSoftware(const AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    avcodec_parameters_to_context(ctx_, st->codecpar);
    ctx_->pkt_timebase = st->time_base;

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        return false;
    }
    hardware_ = false;
    return true;
}

bool VideoDecoder::sendPacket(const AVPacket* pkt) {
    if (!ctx_) return false;
    std::lock_guard<std::recursive_mutex> lk(d3dMutex_);
    int ret = avcodec_send_packet(ctx_, pkt);
    return ret >= 0 || ret == AVERROR(EAGAIN);
}

double VideoDecoder::framePts(const AVFrame* f) const {
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    if (ts == AV_NOPTS_VALUE) return lastPts_ + frameDuration_;
    return ts * av_q2d(timeBase_);
}

// Interlaced streams: one decoded frame holds two fields, so the deinterlacer
// has to run before anything else sees the frame. bwdif in send_frame mode is
// 1-in-1-out (no lookahead), which keeps the decoder's one-frame-in/one-out
// contract and therefore all the timing downstream unchanged.
bool VideoDecoder::buildVideoGraph(AVPixelFormat in) {
    if (vgraph_) {
        avfilter_graph_free(&vgraph_);
        vbufSrc_ = nullptr;
        vbufSink_ = nullptr;
    }
    vgraphIn_ = AV_PIX_FMT_NONE;
    vgraph_ = avfilter_graph_alloc();
    if (!vgraph_) return false;

    // Keep 10-bit depth (bwdif has a yuv420p10le variant); the sink converts back
    // to the semi-planar layouts the renderer uploads without a sws pass.
    const bool tenBit = in == AV_PIX_FMT_P010LE || in == AV_PIX_FMT_YUV420P10LE ||
                        in == AV_PIX_FMT_YUV422P10LE || in == AV_PIX_FMT_YUV444P10LE;
    const char* chain =
        tenBit ? "format=yuv420p10le,bwdif=mode=send_frame:parity=auto:deint=all,format=p010le"
               : "format=yuv420p,bwdif=mode=send_frame:parity=auto:deint=all,format=nv12";

    char args[192];
    std::snprintf(args, sizeof(args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
                  ctx_ ? ctx_->width : 0, ctx_ ? ctx_->height : 0, int(in), timeBase_.num,
                  timeBase_.den);
    if (avfilter_graph_create_filter(&vbufSrc_, avfilter_get_by_name("buffer"), "in", args,
                                     nullptr, vgraph_) < 0 ||
        avfilter_graph_create_filter(&vbufSink_, avfilter_get_by_name("buffersink"), "out",
                                     nullptr, nullptr, vgraph_) < 0) {
        LOGE("video: cannot create the deinterlace graph");
        avfilter_graph_free(&vgraph_);
        vbufSrc_ = nullptr;
        vbufSink_ = nullptr;
        return false;
    }

    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;
    if (avfilter_graph_parse2(vgraph_, chain, &inputs, &outputs) < 0 || !inputs || !outputs) {
        LOGE("video: cannot parse the deinterlace chain");
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&vgraph_);
        vbufSrc_ = nullptr;
        vbufSink_ = nullptr;
        return false;
    }
    const int linkIn = avfilter_link(vbufSrc_, 0, inputs->filter_ctx, inputs->pad_idx);
    const int linkOut = avfilter_link(outputs->filter_ctx, outputs->pad_idx, vbufSink_, 0);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (linkIn < 0 || linkOut < 0 || avfilter_graph_config(vgraph_, nullptr) < 0) {
        LOGE("video: cannot configure the deinterlace graph");
        avfilter_graph_free(&vgraph_);
        vbufSrc_ = nullptr;
        vbufSink_ = nullptr;
        return false;
    }
    vgraphIn_ = in;
    LOGI("video: deinterlacing %dx%d (%s) with bwdif", ctx_ ? ctx_->width : 0,
         ctx_ ? ctx_->height : 0, tenBit ? "10-bit" : "8-bit");
    return true;
}

AVFrame* VideoDecoder::deinterlace(AVFrame* in) {
    if (!vgraph_ || vgraphIn_ != AVPixelFormat(in->format)) {
        if (!buildVideoGraph(AVPixelFormat(in->format))) return in;  // play as stored
    }
    // The container says interlaced even when the frame flag is missing, which
    // is the normal case for hardware decoders; parity=auto needs the flags.
    in->flags |= AV_FRAME_FLAG_INTERLACED;
    if (fieldOrder_ == AV_FIELD_TT || fieldOrder_ == AV_FIELD_TB)
        in->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    else if (fieldOrder_ == AV_FIELD_BB || fieldOrder_ == AV_FIELD_BT)
        in->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;

    const int ret = av_buffersrc_write_frame(vbufSrc_, in);  // takes its own ref
    av_frame_free(&in);
    if (ret < 0) {
        LOGE("video: deinterlace input rejected (%d)", ret);
        return nullptr;
    }
    AVFrame* out = av_frame_alloc();
    if (!out) return nullptr;
    const int got = av_buffersink_get_frame(vbufSink_, out);
    if (got < 0) {
        if (got != AVERROR(EAGAIN)) LOGE("video: deinterlace output failed (%d)", got);
        av_frame_free(&out);
        return nullptr;
    }
    return out;
}

VideoFramePtr VideoDecoder::receiveFrame() {
    if (!ctx_) return nullptr;

    AVFrame* raw = av_frame_alloc();
    int ret;
    {
        std::lock_guard<std::recursive_mutex> lk(d3dMutex_);
        ret = avcodec_receive_frame(ctx_, raw);
    }
    if (ret < 0) {
        av_frame_free(&raw);
        return nullptr;
    }

    // Containers that do not signal the field order (and mixed content) flag
    // individual frames instead; switch the deinterlacer on when that happens.
    if (!interlaced_ && !deintDisabled_ && (raw->flags & AV_FRAME_FLAG_INTERLACED)) {
        interlaced_ = true;
        LOGI("video: frame flagged interlaced, deinterlacing with bwdif");
    }

    auto out = std::make_shared<VideoFrame>();
    out->pts = framePts(raw);
    lastPts_ = out->pts;
    out->format = static_cast<AVPixelFormat>(raw->format);
    out->color = classify(raw);

    // Hardware frames live in a D3D11 texture array. In passthrough mode they go
    // to the renderer untouched (zero-copy); otherwise pull them back to system
    // memory as NV12/P010 and upload.
    //
    // HDR is always pulled back: the video processor's colour space struct
    // cannot describe BT.2020/PQ, and the shader needs the 10-bit planes to do
    // the transfer-function conversion and tone mapping itself.
    AVFrame* src = raw;
    if (raw->format == AV_PIX_FMT_D3D11) {
        if (out->color.hdr()) {
            if (!loggedHdr_) {
                loggedHdr_ = true;
                LOGI("video: HDR source (%s/%s, %.0f nits) -> decoded to 10-bit planes, tone mapping in shader",
                     matrixName(out->color.matrix), trcName(out->color.trc), out->color.peakNits);
            }
        }
        if (passthrough_.load(std::memory_order_relaxed) && !out->color.hdr() && !interlaced_) {
            out->hw = true;
            out->format = AV_PIX_FMT_D3D11;
            out->frame = raw;
            return out;
        }
        AVFrame* sw = av_frame_alloc();
        int xfer = AVERROR(ENOMEM);
        if (sw) {
            std::lock_guard<std::recursive_mutex> lk(d3dMutex_);
            xfer = av_hwframe_transfer_data(sw, raw, 0);
        }
        if (!sw || xfer < 0) {
            if (sw) av_frame_free(&sw);
            av_frame_free(&raw);
            return nullptr;
        }
        sw->pts = raw->pts;
        sw->best_effort_timestamp = raw->best_effort_timestamp;
        // The interlaced / top-field-first flags do not survive the transfer,
        // and bwdif's parity=auto needs them.
        sw->flags = raw->flags;
        av_frame_free(&raw);
        src = sw;
    }

    if (interlaced_) {
        AVFrame* di = deinterlace(src);
        if (!di) return nullptr;
        src = di;
        out->format = static_cast<AVPixelFormat>(src->format);
    }

    // NV12/P010 go straight to the renderer, which converts YUV->RGB (and, for
    // 10-bit, applies PQ/HLG + tone mapping) in the shader.
    if (src->format == AV_PIX_FMT_NV12 || src->format == AV_PIX_FMT_P010LE) {
        out->hw = false;
        out->format = static_cast<AVPixelFormat>(src->format);
        out->frame = src;
        return out;
    }

    // Everything else (YUV420P, 10-bit that we cannot upload, ...) is converted
    // to RGBA once, with the source matrix so colours are at least right.
    if (!sws_) {
        sws_ = sws_getContext(src->width, src->height,
                              static_cast<AVPixelFormat>(src->format),
                              src->width, src->height, AV_PIX_FMT_RGBA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) {
            av_frame_free(&src);
            return nullptr;
        }
        // sws applies the YUV matrix only; primaries and transfer function stay
        // in the source's domain, which is exactly what the video shader wants.
        const int srcCs = out->color.matrix == 2 ? SWS_CS_BT2020
                          : out->color.matrix == 0 ? SWS_CS_ITU601
                                                   : SWS_CS_ITU709;
        sws_setColorspaceDetails(sws_, sws_getCoefficients(srcCs),
                                 out->color.fullRange ? 1 : 0,
                                 sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16,
                                 1 << 16);
    }

    AVFrame* rgba = av_frame_alloc();
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = src->width;
    rgba->height = src->height;
    if (av_frame_get_buffer(rgba, 32) < 0) {
        av_frame_free(&rgba);
        av_frame_free(&src);
        return nullptr;
    }
    sws_scale(sws_, src->data, src->linesize, 0, src->height, rgba->data, rgba->linesize);
    av_frame_free(&src);

    out->hw = false;
    out->frame = rgba;
    out->format = AV_PIX_FMT_RGBA;
    return out;
}

void VideoDecoder::flush() {
    if (ctx_) {
        std::lock_guard<std::recursive_mutex> lk(d3dMutex_);
        avcodec_flush_buffers(ctx_);
    }
    lastPts_ = 0.0;
}

ID3D11Device* VideoDecoder::d3dDevice() const {
#ifdef _WIN32
    if (!hwDeviceCtx_) return nullptr;
    auto* hwdc = reinterpret_cast<AVHWDeviceContext*>(hwDeviceCtx_->data);
    auto* d3d = hwdc ? reinterpret_cast<AVD3D11VADeviceContext*>(hwdc->hwctx) : nullptr;
    return d3d ? d3d->device : nullptr;
#else
    return nullptr;
#endif
}

ID3D11DeviceContext* VideoDecoder::d3dContext() const {
#ifdef _WIN32
    if (!hwDeviceCtx_) return nullptr;
    auto* hwdc = reinterpret_cast<AVHWDeviceContext*>(hwDeviceCtx_->data);
    auto* d3d = hwdc ? reinterpret_cast<AVD3D11VADeviceContext*>(hwdc->hwctx) : nullptr;
    return d3d ? d3d->device_context : nullptr;
#else
    return nullptr;
#endif
}

bool VideoDecoder::d3dLock(void (**lock)(void*), void (**unlock)(void*), void** lockCtx) const {
#ifdef _WIN32
    if (!hwDeviceCtx_) return false;
    *lock = &d3dLockCB;
    *unlock = &d3dUnlockCB;
    *lockCtx = const_cast<std::recursive_mutex*>(&d3dMutex_);
    return true;
#else
    return false;
#endif
}
