#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include "VideoItem.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSize>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <rhi/qrhi.h>

#include "RenderGraph.h"
#include "SamplePass.h"
#include "core/Log.h"
#include "core/Recorder.h"
#include "d3d11/D3D11VideoProcessor.h"
#include "Hdr.h"

#include <QPointer>

namespace {

// Qt renders the window on its render thread; the offline export renders the
// same chain from the GUI thread. Both take this lock, and VideoItem holds it
// for the whole scene-graph frame (beforeRendering..afterRendering), so the two
// never drive the same QRhi/D3D11 device at the same time.
std::recursive_mutex& frameMutex() {
    static std::recursive_mutex m;
    return m;
}

// Neutral 1x1 pixel bound as the second sampler when the shader reads only one
// texture. Uploaded lazily on the first frame (see render()).
constexpr quint8 kBlackPixel[4] = { 0, 0, 0, 255 };

// Draws |text| into a transparent, premultiplied image the size of the video.
// White fill plus a dark outline, which is what libass/mpv do by default and
// what stays readable on bright and dark scenes alike.
QImage renderSubtitleImage(const QString& text, const QSize& size) {
    QImage img(size, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    if (text.isEmpty() || size.isEmpty()) return img;

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int px = std::max(14, int(size.height() * 0.045));
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(px);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);

    const QFontMetrics fm(font);
    const int maxW = int(size.width() * 0.9);

    // Greedy wrapping over break units: a Latin word stays whole, every CJK
    // character is its own break opportunity (CJK has no spaces to break on).
    auto isCjk = [](QChar c) {
        const ushort u = c.unicode();
        return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF) ||
               (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFF60);
    };

    QStringList lines;
    for (const QString& paragraph : text.split(QLatin1Char('\n'))) {
        QStringList units;
        QString word;
        for (const QChar c : paragraph) {
            if (c == QLatin1Char(' ')) {
                if (!word.isEmpty()) {
                    units << word;
                    word.clear();
                }
                units << QStringLiteral(" ");
            } else if (isCjk(c)) {
                if (!word.isEmpty()) {
                    units << word;
                    word.clear();
                }
                units << QString(c);
            } else {
                word += c;
            }
        }
        if (!word.isEmpty()) units << word;

        QString line;
        for (const QString& unit : units) {
            if (unit == QLatin1String(" ") && line.isEmpty()) continue;
            if (!line.isEmpty() && fm.horizontalAdvance(line + unit) > maxW) {
                lines << line.trimmed();
                line = (unit == QLatin1String(" ")) ? QString() : unit;
            } else {
                line += unit;
            }
        }
        if (!line.trimmed().isEmpty()) lines << line.trimmed();
    }
    if (lines.isEmpty()) return img;

    // Avoid a lone orphan glyph on the last line (a single CJK char, or a
    // one-character leftover). Never rebalance otherwise: pulling characters
    // across a break would split an English word.
    if (lines.size() >= 2 && lines.last().size() <= 2) {
        QString& prev = lines[lines.size() - 2];
        QString& last = lines[lines.size() - 1];
        while (prev.size() > 2 && last.size() < 3) {
            last.prepend(prev.right(1));
            prev.chop(1);
        }
    }

    const int lineH = int(fm.height() * 1.18);
    int baseline = size.height() - int(size.height() * 0.05) - lineH * lines.size() + fm.ascent();

    QPainterPath path;
    for (const QString& line : lines) {
        const int w = fm.horizontalAdvance(line);
        path.addText((size.width() - w) / 2, baseline, font, line);
        baseline += lineH;
    }

    QPen outline(QColor(0, 0, 0, 235));
    outline.setWidth(std::max(2, px / 9));
    outline.setJoinStyle(Qt::RoundJoin);
    outline.setCapStyle(Qt::RoundCap);
    p.strokePath(path, outline);
    p.fillPath(path, QColor(255, 255, 255));
    return img;
}

// Frame source selection, one pass chain:
//
//   [zero-copy]  decoded NV12/P010 texture -> ID3D11VideoProcessor -> RGBA
//                texture imported into QRhi, no CPU touch (SDR only: the video
//                processor cannot describe BT.2020/PQ)
//   [upload]     NV12 (8-bit) or P010 (10-bit) planes uploaded per frame; the
//                shader does YUV->RGB, PQ/HLG decode, tone mapping and gamut
//
// and then, if an effect is enabled:
//
//   video pass (source -> RGBA at video size) -> effect pass -> present pass
//
// Without effects the present pass samples the source directly, so the default
// path costs exactly what it did before the graph existed.
class VideoRenderer : public QQuickRhiItemRenderer {
public:
    ~VideoRenderer() override {
        releaseZeroCopy();
        // The item may outlive us (device loss, window teardown): drop its
        // pointer so the offscreen driver never calls into freed state.
        if (handle_ && handle_->load() == this) handle_->store(nullptr);
    }

    // Shared with VideoItem: it needs to drive frames while the window is
    // minimised and Qt stops calling render().
    void setHandle(std::shared_ptr<std::atomic<QQuickRhiItemRenderer*>> h) { handle_ = h; }
    // Renders one frame outside Qt's frame: no swapchain. Returns false when the
    // device is busy with Qt's own frame (the caller retries).
    bool renderOffscreen(QQuickRhiItem* item);

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void rebuildChain();
    void ensureUploadTextures(int w, int h, bool rgba, bool tenBit);
    bool ensureZeroCopy(const AVFrame* f);
    void releaseZeroCopy();
    void releaseResources();
    void frameUpdate(QRhiResourceUpdateBatch* rub);
    // Schedules the GPU->CPU readback of the record pass output (P5). Runs after
    // every pass, so it copies this frame's pixels.
    void recordReadback(QRhiResourceUpdateBatch* rub);
    // Logs the frame's colour description once per distinct value.
    void logColor(const VideoFrame& f);
    // Colour description for the video/present passes.
    ColorParams colorParamsFor(const VideoFrame& f) const;

    QRhi* rhi_ = nullptr;
    std::unique_ptr<RenderGraph> graph_;
    SamplePass* present_ = nullptr;
    SamplePass* videoPass_ = nullptr;
    SamplePass* effectPass_ = nullptr;
    SamplePass* compositePass_ = nullptr;
    SamplePass* scalePass_ = nullptr;
    SamplePass* recordPass_ = nullptr;
    bool chainEffects_ = false;
    bool chainSubs_ = false;
    bool chainScale_ = false;
    // Recording inserts an extra branch at the end of the chain: the preview
    // still samples the chain output, the record pass copies it into an RGBA8
    // texture that is read back for the encoder.
    bool chainRecord_ = false;
    // True when the video pass has to build a display-ready RGB texture for the
    // chain. Zero-copy output is already RGBA, so the pass is skipped there.
    bool chainNeedVideo_ = false;
    QSize chainSize_;
    // Content rectangle in device pixels. The scale pass writes this size when
    // subtitles follow it, so the composite can run at output resolution.
    QSize chainOut_;

    // Subtitle overlay: rasterised on the render thread when the cue changes,
    // uploaded as an RGBA texture and composited at output resolution.
    std::unique_ptr<QRhiTexture> subTex_;
    QSize subTexSize_;
    QImage subImage_;
    QString subText_;
    QString renderedSubText_;
    bool subsActive_ = false;
    bool subDirty_ = false;

    std::unique_ptr<QRhiTexture> yTex_;
    std::unique_ptr<QRhiTexture> uvTex_;
    std::unique_ptr<QRhiTexture> dummyTex_;
    bool dummyUploaded_ = false;
    std::unique_ptr<QRhiTexture> importedTex_[D3D11VideoProcessor::kOutputCount];

    D3D11VideoProcessor* vp_ = nullptr;
    std::unique_ptr<D3D11VideoProcessor> vpOwner_;
    // Native textures opened on the RHI device through the shared NT handles.
    // QRhiTexture::createFrom() does not AddRef, so these must outlive
    // importedTex_ and are released after it.
    ID3D11Texture2D* importedNative_[D3D11VideoProcessor::kOutputCount] = { nullptr, nullptr };
    ID3D11Device5* rhiDev5_ = nullptr;
    ID3D11DeviceContext4* rhiCtx4_ = nullptr;
    ID3D11Fence* consumerFence_ = nullptr;   // signalled by us, after a frame
    HANDLE consumerFenceHandle_ = nullptr;
    ID3D11Fence* producerFence_ = nullptr;   // signalled by the video processor
    uint64_t zcFrame_ = 0;
    bool vpFailed_ = false;
    bool deviceLostLogged_ = false;

    // Screenshot: a pending path claimed from the item, plus the readback that
    // writes it (same chain as a recording, so HDR is tone mapped the same way).
    QString shotFile_;
    bool capturePending_ = false;

    // Texture currently holding the frame, and how the shader must read it.
    QRhiTexture* curA_ = nullptr;
    QRhiTexture* curB_ = nullptr;
    bool rgba_ = false;
    // True only for the zero-copy VideoProcessor output: RGB that is already
    // display-referred sRGB (unlike the sws upload, which keeps PQ/HLG).
    bool zc_ = false;
    int srcW_ = 0, srcH_ = 0;

    // Offscreen driving (P5.2). Qt stops rendering a minimised window, which
    // used to pause the recording; the item's tick calls renderOffscreen()
    // instead. The stand-in target replaces the swapchain for the preview passes
    // (they are not read back), while the chain keeps its video-sized targets.
    std::shared_ptr<std::atomic<QQuickRhiItemRenderer*>> handle_;
    std::unique_ptr<QRhiTexture> offscreenTex_;
    std::unique_ptr<QRhiTextureRenderTarget> offscreenRt_;
    bool offscreen_ = false;
    // Serialises Qt's frames (render thread) against offscreen frames (GUI
    // thread): both drive the same D3D11 device, and a minimised window can be
    // restored in the middle of an offscreen frame. VideoItem holds the same
    // lock for the whole scene-graph frame (see its constructor).

    int vw_ = 0, vh_ = 0;
    bool uploadRgba_ = false;
    bool uploadTenBit_ = false;

    VideoFramePtr frame_;
    Player* player_ = nullptr;
    const AVFrame* uploaded_ = nullptr;
    const AVFrame* pendingUpload_ = nullptr;
    const AVFrame* loggedUpload_ = nullptr;   // PLAY_DEBUG_UPLOAD only

    // Last logged colour description (matrix/primaries/trc/range/peak), so the
    // log reports a change once instead of every frame.
    ColorInfo loggedColor_;
    bool haveLoggedColor_ = false;

    // Present-rate stats (P1 acceptance criteria: no dropped frames at 4K60).
    VideoFramePtr statLast_;
    QElapsedTimer statTimer_;
    int statFrames_ = 0;

    QSize itemSize_;
    float scaleX_ = 1.f;
    float scaleY_ = 1.f;

    // Resampling: filter picked by the user, tap spacing (source texels per
    // output pixel) for the current window size.
    int scalerMode_ = Scaler::Bilinear;
    // Display matrix of the current stream (phone videos): clockwise quarter
    // turns to apply while sampling, plus an optional horizontal mirror.
    int rotation_ = 0;
    bool mirror_ = false;
    float spacingX_ = 1.f;
    float spacingY_ = 1.f;

    // Output colour space. Qt creates a scRGB (FP16 linear) swapchain only when
    // the window's display has Windows HDR switched on; otherwise everything is
    // tone mapped to SDR as before.
    QQuickWindow* window_ = nullptr;
    QPointer<VideoItem> item_;
    bool itemHdr_ = false;
    bool hdrChecked_ = false;
    bool hdrOutput_ = false;
    bool chainHdr_ = false;
    float hdrSdrWhite_ = Hdr::kDefaultSdrWhiteNits;
    float hdrPeakNits_ = Hdr::kDefaultPeakNits;

    // Placeholder effect parameters; the (deferred) beauty chain will drive
    // these.
    float saturation_ = 1.f;
    float brightness_ = 0.f;

    // Recording: one readback per newly presented frame, tagged with that
    // frame's timestamp. The recorder outlives stopRecording() through the
    // shared_ptr captured by the readback callback.
    std::shared_ptr<Recorder> recorder_;
    VideoFramePtr recordedFrame_;
    bool recordPending_ = false;
    double recordPts_ = 0.0;
    // Texture the readback copies: the record pass output in HDR, the chain
    // output itself in SDR (it is already RGBA8 sRGB at video size).
    QRhiTexture* recordTex_ = nullptr;
};

void VideoRenderer::initialize(QRhiCommandBuffer* cb) {
    if (graph_ && graph_->ready()) return;
    rhi_ = rhi();
    LOGI("render: initialize %p (rhi backend=%d)", (void*)this, int(rhi_->backend()));
    if (auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi_->nativeHandles()))
        LOGI("render: rhi d3d11 device=%p context=%p", nh->dev, nh->context);

    graph_ = std::make_unique<RenderGraph>();
    graph_->init(rhi_);
    chainEffects_ = chainSubs_ = chainScale_ = chainNeedVideo_ = false;
    chainHdr_ = false;
    chainSize_ = QSize();
    rebuildChain();

    dummyTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    dummyTex_->create();
    dummyUploaded_ = false;

    // Registered once: every run() hands the first pass's update batch to it.
    graph_->setFrameUpdate([this](QRhiResourceUpdateBatch* rub) { frameUpdate(rub); });
    // ... and the recording readback runs after the passes (see recordReadback).
    graph_->setPostUpdate([this](QRhiResourceUpdateBatch* rub) { recordReadback(rub); });

    deviceLostLogged_ = false;
}

void VideoRenderer::frameUpdate(QRhiResourceUpdateBatch* rub) {
    if (!dummyUploaded_ && dummyTex_) {
        QRhiTextureSubresourceUploadDescription d(kBlackPixel, sizeof(kBlackPixel));
        d.setDataStride(4);
        d.setSourceSize(QSize(1, 1));
        rub->uploadTexture(dummyTex_.get(),
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
        dummyUploaded_ = true;
    }

    const AVFrame* f = pendingUpload_;
    if (f) {
        pendingUpload_ = nullptr;
        const int strideY = f->linesize[0];
        if (qEnvironmentVariableIsSet("PLAY_DEBUG_UPLOAD") && f != loggedUpload_) {
            loggedUpload_ = f;
            const auto* yb = reinterpret_cast<const unsigned char*>(f->data[0]);
            const auto* ub = reinterpret_cast<const unsigned char*>(f->data[1]);
            LOGI("upload: fmt=%d %dx%d y stride=%d first=%02x%02x uv=%p stride=%d first=%02x%02x",
                 int(f->format), f->width, f->height, strideY, yb[0], yb[1],
                 (const void*)ub, f->linesize[1], ub ? ub[0] : 0, ub ? ub[1] : 0);
        }
        QRhiTextureSubresourceUploadDescription dy(f->data[0],
                                                   quint32(strideY) * quint32(f->height));
        dy.setDataStride(quint32(strideY));
        dy.setSourceSize(QSize(f->width, f->height));
        rub->uploadTexture(yTex_.get(),
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, dy)));

        if (uvTex_ && f->data[1]) {
            const int strideUv = f->linesize[1];
            const int h = (f->height + 1) / 2;
            QRhiTextureSubresourceUploadDescription duv(f->data[1], quint32(strideUv) * quint32(h));
            duv.setDataStride(quint32(strideUv));
            duv.setSourceSize(QSize((f->width + 1) / 2, h));
            rub->uploadTexture(uvTex_.get(),
                               QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, duv)));
        }
        uploaded_ = f;
    }

    if (subDirty_ && subTex_ && !subImage_.isNull()) {
        QRhiTextureSubresourceUploadDescription d(subImage_.constBits(), subImage_.sizeInBytes());
        d.setDataStride(quint32(subImage_.bytesPerLine()));
        d.setSourceSize(subImage_.size());
        rub->uploadTexture(subTex_.get(),
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
        // subImage_ stays alive until the batch is submitted below, so the
        // pointer handed to QRhi is valid for the whole call.
        subDirty_ = false;
    }
}

void VideoRenderer::recordReadback(QRhiResourceUpdateBatch* rub) {
    if (!recordPending_) return;
    QRhiTexture* tex = recordTex_;
    auto rec = recorder_;
    const bool capture = capturePending_ && !shotFile_.isEmpty();
    if (!tex || (!rec && !capture)) {
        // A rebuilt chain creates its textures on the next prepare(): keep the
        // request pending and take it on the following frame.
        return;
    }
    recordPending_ = false;
    const QString shotPath = shotFile_;
    if (capture) {
        capturePending_ = false;
        shotFile_.clear();
    }

    const QSize size = tex->pixelSize();
    const double pts = recordPts_;
    QQuickRhiItem* item = item_;
    auto* result = new QRhiReadbackResult;
    result->completed = [rec, result, pts, size, capture, shotPath, item] {
        // An RGBA8 target comes back tightly packed, top row first.
        const qsizetype need = qsizetype(size.width()) * size.height() * 4;
        bool ok = result->data.size() >= need;
        if (ok && capture) {
            // Same pixels the encoder would get: display-referred sRGB, already
            // tone mapped for HDR. No window chrome, no letterbox.
            QImage img(reinterpret_cast<const uchar*>(result->data.constData()), size.width(),
                       size.height(), size.width() * 4, QImage::Format_RGBA8888);
            ok = img.save(shotPath, "PNG");
            if (item) {
                QMetaObject::invokeMethod(
                    item,
                    [item, shotPath, ok] {
                        auto* vi = static_cast<VideoItem*>(item);
                        if (vi) emit vi->screenshotSaved(shotPath, ok);
                    },
                    Qt::QueuedConnection);
            } else if (!ok) {
                LOGW("shot: cannot save %s", qPrintable(shotPath));
            }
        }
        if (ok && rec) {
            // The encoder gets the same pixels; a screenshot taken while
            // recording does not disturb the file.
            rec->pushVideo(std::move(result->data), size.width(), size.height(), pts);
        } else if (!ok) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOGW("record: short readback (%lld of %lld bytes)", (long long)result->data.size(),
                     (long long)need);
            }
        }
        delete result;
    };
    rub->readBackTexture(QRhiReadbackDescription(tex), result);
}

void VideoRenderer::rebuildChain() {
    if (!graph_) return;
    graph_->clearPasses();
    present_ = videoPass_ = effectPass_ = compositePass_ = scalePass_ = recordPass_ = nullptr;

    const bool effects = chainEffects_ && chainSize_.width() > 0;
    const bool subs = chainSubs_ && chainSize_.width() > 0;
    const bool scale = chainScale_ && chainSize_.width() > 0;
    // scRGB needs the headroom between passes, otherwise the video pass would
    // clip the highlights it just decoded.
    const QRhiTexture::Format chainFormat =
        hdrOutput_ ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    const bool rec = chainRecord_ && chainSize_.width() > 0;

    if (chainNeedVideo_ && (effects || subs || scale || rec)) {
        auto video = std::make_unique<SamplePass>(graph_.get(), "video", ":/shaders/video.frag.qsb",
                                                  chainSize_, chainFormat);
        videoPass_ = video.get();
        graph_->addPass(std::move(video));
    }

    if (effects) {
        auto effect = std::make_unique<SamplePass>(graph_.get(), "effect",
                                                   ":/shaders/effect.frag.qsb", chainSize_,
                                                   chainFormat);
        effectPass_ = effect.get();
        graph_->addPass(std::move(effect));
    }

    if (rec) {
        // Recording rewrites the layout: the chain ends in an offscreen copy at
        // video resolution (subtitles burned in at that size, not at window
        // size), the recorder reads that copy back, and the preview letterboxes
        // it afterwards. The file therefore never contains the window layout.
        if (subs) {
            auto composite = std::make_unique<SamplePass>(graph_.get(), "composite",
                                                          ":/shaders/composite.frag.qsb",
                                                          chainSize_, chainFormat);
            compositePass_ = composite.get();
            graph_->addPass(std::move(composite));
        }
        if (hdrOutput_) {
            // The chain is linear scRGB (FP16): tone map it back to 8-bit SDR for
            // the encoder. In SDR the chain output already is RGBA8 sRGB, so the
            // readback takes it directly and no extra full-frame pass runs.
            auto r = std::make_unique<SamplePass>(graph_.get(), "record",
                                                  ":/shaders/record.frag.qsb", chainSize_,
                                                  QRhiTexture::RGBA8);
            recordPass_ = r.get();
            graph_->addPass(std::move(r));
        }
        if (scale) {
            auto s = std::make_unique<SamplePass>(graph_.get(), "scale", ":/shaders/scale.frag.qsb",
                                                  QSize(), chainFormat);
            scalePass_ = s.get();
            graph_->addPass(std::move(s));
        } else {
            auto present =
                std::make_unique<SamplePass>(graph_.get(), "present", ":/shaders/video.frag.qsb");
            present_ = present.get();
            graph_->addPass(std::move(present));
        }
    } else {
        if (scale) {
            // Without subtitles the resampler writes straight into the swapchain: the
            // vertex shader applies the letterbox scale, so one pass replaces the
            // present pass. With subtitles it has to write an offscreen texture the
            // size of the content rectangle, so the composite can run at output
            // resolution (crisp text instead of text resampled with the video).
            const QSize target = subs && chainOut_.width() > 0 ? chainOut_ : QSize();
            auto s = std::make_unique<SamplePass>(graph_.get(), "scale", ":/shaders/scale.frag.qsb",
                                                  target, chainFormat);
            scalePass_ = s.get();
            graph_->addPass(std::move(s));
        }

        if (subs) {
            // Last pass: letterbox in the vertex stage, blend the overlay 1:1.
            auto composite = std::make_unique<SamplePass>(graph_.get(), "composite",
                                                          ":/shaders/composite.frag.qsb", QSize(),
                                                          chainFormat);
            compositePass_ = composite.get();
            graph_->addPass(std::move(composite));
        } else if (!scale) {
            auto present =
                std::make_unique<SamplePass>(graph_.get(), "present", ":/shaders/video.frag.qsb");
            present_ = present.get();
            graph_->addPass(std::move(present));
        }
    }

    LOGI("render: chain video=%d effect=%d composite=%d scale=%s record=%d %dx%d %s%s",
         chainNeedVideo_ ? 1 : 0, effects ? 1 : 0, subs ? 1 : 0,
         scale ? Scaler::name(scalerMode_) : "off", rec ? 1 : 0, chainSize_.width(),
         chainSize_.height(), hdrOutput_ ? "hdr16f" : "sdr8",
         subs && scale && !rec ? qPrintable(QStringLiteral(" out=%1x%2").arg(chainOut_.width()).arg(
                                      chainOut_.height()))
                               : "");
}

void VideoRenderer::ensureUploadTextures(int w, int h, bool rgba, bool tenBit) {
    if (yTex_ && w == vw_ && h == vh_ && rgba == uploadRgba_ && tenBit == uploadTenBit_) return;
    if (w <= 0 || h <= 0) return;

    vw_ = w;
    vh_ = h;
    uploadRgba_ = rgba;
    uploadTenBit_ = tenBit;

    if (rgba) {
        yTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(w, h)));
        uvTex_.reset();
    } else if (tenBit) {
        // P010: 10 bits in the high bits of 16-bit words. Sampling an R16/RG16
        // texture normalises to 0..1, which is all the shader needs (the range
        // constants are the same as for 8-bit).
        yTex_.reset(rhi_->newTexture(QRhiTexture::R16, QSize(w, h)));
        uvTex_.reset(rhi_->newTexture(QRhiTexture::RG16, QSize((w + 1) / 2, (h + 1) / 2)));
    } else {
        yTex_.reset(rhi_->newTexture(QRhiTexture::R8, QSize(w, h)));
        uvTex_.reset(rhi_->newTexture(QRhiTexture::RG8, QSize((w + 1) / 2, (h + 1) / 2)));
    }
    yTex_->create();
    if (uvTex_) uvTex_->create();
    uploaded_ = nullptr;
}

bool VideoRenderer::ensureZeroCopy(const AVFrame* f) {
    if (vp_ && importedTex_[0]) return true;
    if (vpFailed_ || !player_) return false;

    // The scene graph's device is not created with VIDEO_SUPPORT, so the
    // decoder keeps its own device and the RGBA result crosses devices through
    // a shared NT handle. Both devices must stay alive for the whole session.
    const auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi_->nativeHandles());
    if (!nh || !nh->dev || !nh->context) {
        LOGW("zero-copy: no native D3D11 device on the RHI, using the copy path");
        vpFailed_ = true;
        return false;
    }

    auto* tex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    if (!tex) {
        vpFailed_ = true;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) {
        LOGW("zero-copy: unsupported dxgi format %d", int(desc.Format));
        vpFailed_ = true;
        return false;
    }

    vpOwner_ = std::make_unique<D3D11VideoProcessor>();
    vp_ = vpOwner_.get();
    std::string err;
    // Use the *display* size, not the texture size: D3D11 textures are usually
    // height-aligned (1080 -> 1088) and the video processor handles the crop.
    const int outW = f->width > 0 ? f->width : int(desc.Width);
    const int outH = f->height > 0 ? f->height : int(desc.Height);
    if (!vp_->init(player_->d3dDevice(), player_->d3dContext(),
                   int(desc.Width), int(desc.Height), outW, outH, desc.Format, &err)) {
        LOGE("zero-copy unavailable: %s", err.c_str());
        vpOwner_.reset();
        vp_ = nullptr;
        vpFailed_ = true;
        return false;
    }

    // Share FFmpeg's recursive lock: the immediate context is used by the decode
    // thread too, and D3D11 requires serialised access to it.
    void (*lockFn)(void*) = nullptr;
    void (*unlockFn)(void*) = nullptr;
    void* lockCtx = nullptr;
    if (player_->d3dLock(&lockFn, &unlockFn, &lockCtx)) {
        vp_->setLock(lockFn, unlockFn, lockCtx);
    } else {
        LOGW("zero-copy: no decoder lock available, running unlocked");
    }

    auto rd = static_cast<ID3D11Device*>(nh->dev);
    auto rc = static_cast<ID3D11DeviceContext*>(nh->context);

    // Fences: the producer signals after every blt, we signal when a frame has
    // been consumed. Both waits are GPU-side, so neither thread stalls.
    if (FAILED(rd->QueryInterface(IID_PPV_ARGS(&rhiDev5_))) ||
        FAILED(rc->QueryInterface(IID_PPV_ARGS(&rhiCtx4_)))) {
        LOGW("zero-copy: RHI device has no D3D11.4 fence support");
        releaseZeroCopy();
        vpFailed_ = true;
        return false;
    }
    if (FAILED(rhiDev5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                     IID_PPV_ARGS(&consumerFence_))) ||
        FAILED(consumerFence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                  &consumerFenceHandle_))) {
        LOGW("zero-copy: cannot create the consumer fence");
        releaseZeroCopy();
        vpFailed_ = true;
        return false;
    }
    if (!vp_->setConsumerFence(consumerFenceHandle_) ||
        FAILED(rhiDev5_->OpenSharedFence(vp_->producerFenceHandle(),
                                         IID_PPV_ARGS(&producerFence_)))) {
        LOGW("zero-copy: cannot exchange fences between the two devices");
        releaseZeroCopy();
        vpFailed_ = true;
        return false;
    }

    ID3D11Device1* rhiDev1 = nullptr;
    if (FAILED(rd->QueryInterface(IID_PPV_ARGS(&rhiDev1)))) {
        LOGW("zero-copy: ID3D11Device1 unavailable on the RHI device");
        releaseZeroCopy();
        vpFailed_ = true;
        return false;
    }

    for (int i = 0; i < D3D11VideoProcessor::kOutputCount; ++i) {
        HANDLE handle = vp_->outputHandle(i);
        if (!handle ||
            FAILED(rhiDev1->OpenSharedResource1(handle, IID_PPV_ARGS(&importedNative_[i])))) {
            LOGE("zero-copy: OpenSharedResource1 failed");
            rhiDev1->Release();
            releaseZeroCopy();
            vpFailed_ = true;
            return false;
        }
        importedTex_[i].reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(outW, outH)));
        QRhiTexture::NativeTexture native{};
        native.object = reinterpret_cast<quint64>(importedNative_[i]);
        native.layout = 0;
        if (!importedTex_[i]->createFrom(native)) {
            LOGE("zero-copy: QRhiTexture::createFrom failed");
            rhiDev1->Release();
            releaseZeroCopy();
            vpFailed_ = true;
            return false;
        }
    }
    rhiDev1->Release();
    zcFrame_ = 0;

    LOGI("zero-copy: enabled (%dx%d, dxgi fmt %d), shared across devices %p -> %p",
         outW, outH, int(desc.Format), player_->d3d11DeviceRaw(), nh->dev);
    return true;
}

void VideoRenderer::releaseZeroCopy() {
    // Drop the QRhi wrappers first: createFrom() does not AddRef the native
    // texture, so the D3D11 object must outlive them.
    for (auto& t : importedTex_) t.reset();
    for (auto& n : importedNative_) {
        if (n) { n->Release(); n = nullptr; }
    }
    if (producerFence_) { producerFence_->Release(); producerFence_ = nullptr; }
    if (consumerFenceHandle_) { CloseHandle(consumerFenceHandle_); consumerFenceHandle_ = nullptr; }
    if (consumerFence_) { consumerFence_->Release(); consumerFence_ = nullptr; }
    if (rhiCtx4_) { rhiCtx4_->Release(); rhiCtx4_ = nullptr; }
    if (rhiDev5_) { rhiDev5_->Release(); rhiDev5_ = nullptr; }
    vpOwner_.reset();
    vp_ = nullptr;
    zcFrame_ = 0;
}

void VideoRenderer::releaseResources() {
    releaseZeroCopy();
    if (graph_) graph_->release();
    graph_.reset();
    present_ = videoPass_ = effectPass_ = compositePass_ = scalePass_ = recordPass_ = nullptr;
    yTex_.reset();
    uvTex_.reset();
    dummyTex_.reset();
    subTex_.reset();
    subTexSize_ = QSize();
    subImage_ = QImage();
    renderedSubText_.clear();
    subDirty_ = false;

    curA_ = curB_ = nullptr;
    uploaded_ = nullptr;
    pendingUpload_ = nullptr;
    loggedUpload_ = nullptr;
    dummyUploaded_ = false;
    vw_ = vh_ = 0;
    srcW_ = srcH_ = 0;
    uploadTenBit_ = false;
    haveLoggedColor_ = false;
    chainEffects_ = chainSubs_ = chainScale_ = chainNeedVideo_ = false;
    chainRecord_ = false;
    recordPending_ = false;
    recordedFrame_.reset();
    recordTex_ = nullptr;
    chainHdr_ = false;
    chainSize_ = QSize();
    vpFailed_ = false;
}

void VideoRenderer::synchronize(QQuickRhiItem* item) {
    std::lock_guard<std::recursive_mutex> frameLock(frameMutex());
    auto* vi = static_cast<VideoItem*>(item);
    player_ = vi->player();
    itemSize_ = QSize(static_cast<int>(vi->width()), static_cast<int>(vi->height()));
    saturation_ = static_cast<float>(vi->saturation());
    brightness_ = static_cast<float>(vi->brightness());
    scalerMode_ = vi->scalerMode();
    window_ = vi->window();
    item_ = vi;
    itemHdr_ = vi->hdr();
    // A screenshot request is claimed here: the GUI thread only queues it. Do
    // not clear the pending path on later frames — the readback may need one
    // more frame before the chain textures exist.
    if (const QString req = vi->takeScreenshotRequest(); !req.isEmpty()) {
        shotFile_ = req;
        capturePending_ = true;
        LOGI("shot: renderer claimed %s", qPrintable(shotFile_));
    }
    rotation_ = player_ ? player_->rotation() : 0;
    mirror_ = player_ && player_->mirrored();
    // A reference for the whole frame: stopRecording() can run on the GUI thread
    // while this one is still pushing.
    recorder_ = player_ ? player_->recorder() : nullptr;

    if (player_) {
        VideoFramePtr f;
        if (vi->offlineExport()) {
            // The exporter owns the frame order; the clock is not involved.
            f = vi->offlineFrame();
        } else {
            f = player_->frameForNow();
        }
        if (f && f->frame) frame_ = f;
        if (frame_ && frame_->frame) logColor(*frame_);

        // Subtitles follow the master clock, not the frame pts: during a seek
        // the cue must switch as soon as the position does. Offline there is no
        // clock, so use the frame's own timestamp.
        const double subClock =
            (vi->offlineExport() && f && f->frame) ? f->pts : player_->position();
        subText_ = vi->subtitlesEnabled() ? vi->subtitleTextAt(subClock) : QString();
        subsActive_ = vi->subtitlesEnabled() && vi->hasSubtitles();

        if (f && f->frame && f != statLast_) {
            statLast_ = f;
            if (!statTimer_.isValid()) {
                statTimer_.start();
            } else if (statTimer_.elapsed() >= 5000) {
                const double secs = statTimer_.elapsed() / 1000.0;
                const VideoFramePtr cur = player_->currentFrame();
                LOGI("render: %p %.1f fps presented (%d frames in %.1fs, dropped=%llu, underruns=%llu, "
                     "pos=%.3f pts=%.3f)",
                     (void*)this, statFrames_ / secs, statFrames_, secs,
                     static_cast<unsigned long long>(player_->droppedFrames()),
                     static_cast<unsigned long long>(player_->audioUnderruns()),
                     player_->position(), cur ? cur->pts : -1.0);
                statTimer_.restart();
                statFrames_ = 0;
            }
            ++statFrames_;
        }
    }

    if (frame_ && frame_->frame && itemSize_.width() > 0 && itemSize_.height() > 0) {
        // A 90/270 degree display matrix swaps the presented aspect ratio.
        const float fw = static_cast<float>(frame_->frame->width);
        const float fh = static_cast<float>(frame_->frame->height);
        const float va = (rotation_ & 1) ? fh / fw : fw / fh;
        const float ia = static_cast<float>(itemSize_.width()) / itemSize_.height();
        if (va > ia) {
            scaleX_ = 1.f;
            scaleY_ = ia / va;
        } else {
            scaleX_ = va / ia;
            scaleY_ = 1.f;
        }
    }
}

void VideoRenderer::logColor(const VideoFrame& f) {
    const ColorInfo& c = f.color;
    if (haveLoggedColor_ && c.matrix == loggedColor_.matrix && c.primaries == loggedColor_.primaries &&
        c.trc == loggedColor_.trc && c.fullRange == loggedColor_.fullRange &&
        c.peakNits == loggedColor_.peakNits) {
        return;
    }
    loggedColor_ = c;
    haveLoggedColor_ = true;
    static const char* kMatrix[] = { "bt601", "bt709", "bt2020nc" };
    static const char* kTrc[] = { "sdr", "pq", "hlg" };
    static const char* kPrim[] = { "bt709", "bt2020" };
    LOGI("colour: %s/%s %s%s, peak %.0f nits%s",
         kMatrix[c.matrix >= 0 && c.matrix <= 2 ? c.matrix : 1],
         kPrim[c.primaries > 0 ? 1 : 0], kTrc[c.trc >= 0 && c.trc <= 2 ? c.trc : 0],
         c.fullRange ? " full-range" : " limited-range", double(c.peakNits),
         c.hdr() ? " (tone mapped to SDR)" : "");
}

ColorParams VideoRenderer::colorParamsFor(const VideoFrame& f) const {
    ColorParams p;
    p.mode = 0;
    p.matrix = f.color.matrix;
    p.primaries = f.color.primaries;
    p.trc = f.color.trc;
    p.fullRange = f.color.fullRange;
    p.peakNits = f.color.peakNits;
    p.hdrOut = hdrOutput_;
    return p;
}

void VideoRenderer::render(QRhiCommandBuffer* cb) {
    std::lock_guard<std::recursive_mutex> frameLock(frameMutex());
    // A lost device (driver reset, TDR) invalidates every QRhi resource we own.
    // Drop them while the RHI object still exists; Qt will call initialize()
    // again on a fresh device.
    if (rhi_->isDeviceLost()) {
        if (!deviceLostLogged_) {
            LOGW("render: device lost, releasing RHI resources");
            deviceLostLogged_ = true;
        }
        releaseResources();
        return;
    }
    if (!graph_ || !graph_->ready()) initialize(cb);

    // Resolve the output colour space once: Qt requested scRGB when --hdr was
    // given, but only keeps it if the display actually has HDR switched on.
    if (!hdrChecked_ && window_ && window_->swapChain()) {
        hdrChecked_ = true;
        QRhiSwapChain* sc = window_->swapChain();
        hdrOutput_ = sc->format() != QRhiSwapChain::SDR;
        const bool forced = qEnvironmentVariableIsSet("PLAY_HDR_FORCE");
        if (forced) {
            // Development only: run the scRGB maths on an SDR display (the
            // image is brighter than the source, but the values are checkable).
            hdrOutput_ = true;
            LOGW("hdr: PLAY_HDR_FORCE is set, treating the SDR swapchain as scRGB");
        }
        if (hdrOutput_) {
            const QRhiSwapChainHdrInfo info = sc->hdrInfo();
            if (info.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits) {
                if (info.limits.luminanceInNits.maxLuminance > 1.f)
                    hdrPeakNits_ = info.limits.luminanceInNits.maxLuminance;
            } else if (info.limits.colorComponentValue.maxColorComponentValue > 0.f) {
                hdrPeakNits_ =
                    info.limits.colorComponentValue.maxColorComponentValue * Hdr::kScRgbWhiteNits;
            }
            if (info.sdrWhiteLevel > 1.f) hdrSdrWhite_ = info.sdrWhiteLevel;
            // An SDR swapchain reports placeholder limits (80 nits white), so
            // the forced path falls back to BT.2408 graphics white. Real HDR
            // output trusts the system value; PLAY_HDR_WHITE overrides both.
            if (forced && hdrSdrWhite_ <= Hdr::kScRgbWhiteNits + 0.5f)
                hdrSdrWhite_ = Hdr::kDefaultSdrWhiteNits;
            const float whiteEnv = qEnvironmentVariable("PLAY_HDR_WHITE").toFloat();
            if (whiteEnv > 1.f) hdrSdrWhite_ = whiteEnv;
            LOGI("hdr: scRGB output, SDR white %.0f nits, display peak %.0f nits",
                 double(hdrSdrWhite_), double(hdrPeakNits_));
        } else {
            LOGI("hdr: SDR output (Windows HDR off, or the display does not support it)");
        }

        // Only scRGB needs the FP16 headroom, so tell the item to follow what
        // the swapchain really is: an SDR window keeps the cheaper RGBA8 buffer.
        if (item_ && itemHdr_ != hdrOutput_) {
            itemHdr_ = hdrOutput_;
            const bool on = hdrOutput_;
            QMetaObject::invokeMethod(
                item_, [item = item_, on] { if (item) item->setHdr(on); },
                Qt::QueuedConnection);
        }
    }

    bool haveFrame = false;

    if (frame_ && frame_->frame) {
        const AVFrame* f = frame_->frame;

        if (frame_->hw) {
            // Zero copy: NV12/P010 stays in VRAM, converted by the video processor.
            if (ensureZeroCopy(f)) {
                vp_->setInputColorSpace(frame_->color.matrix, frame_->color.fullRange);
                auto* tex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
                const int slice = static_cast<int>(reinterpret_cast<intptr_t>(f->data[1]));
                // Everything submitted on this context before now is done as far
                // as the GPU is concerned, so frame zcFrame_-2's buffers may be
                // reused by the video processor.
                rhiCtx4_->Signal(consumerFence_, zcFrame_);
                std::string err;
                const uint64_t consumed = zcFrame_ > 0 ? zcFrame_ - 1 : 0;
                if (vp_->convert(tex, slice, consumed, &err)) {
                    // Sample only after the blt for this frame has finished.
                    rhiCtx4_->Wait(producerFence_, vp_->producedValue());
                    curA_ = importedTex_[vp_->outputIndex()].get();
                    curB_ = dummyTex_.get();
                    rgba_ = true;
                    zc_ = true;  // VideoProcessor output: display-referred sRGB
                    srcW_ = f->width > 0 ? f->width : curA_->pixelSize().width();
                    srcH_ = f->height > 0 ? f->height : curA_->pixelSize().height();
                    haveFrame = true;
                    ++zcFrame_;
                } else {
                    LOGE("zero-copy: convert failed: %s", err.c_str());
                }
            } else if (player_) {
                // Fall back to the upload path for the rest of this file.
                player_->setHardwarePassthrough(false);
            }
        } else {
            const bool tenBit = (frame_->format == AV_PIX_FMT_P010LE);
            const bool isRgba = (frame_->format != AV_PIX_FMT_NV12 && !tenBit);
            ensureUploadTextures(f->width, f->height, isRgba, tenBit);
            curA_ = yTex_.get();
            curB_ = isRgba ? dummyTex_.get() : uvTex_.get();
            rgba_ = isRgba;
            zc_ = false;  // sws RGBA keeps the source transfer function
            srcW_ = f->width;
            srcH_ = f->height;
            haveFrame = true;
        }
    }

    // Effects and subtitles run at video resolution; without them the present
    // pass samples the source directly. A non-bilinear scaler adds a resampling
    // pass that writes into the swapchain, but only when the content rectangle
    // is not already 1:1 with the video.
    const bool wantEffects = saturation_ != 1.f || brightness_ != 0.f;
    const bool wantSubs = subsActive_ && haveFrame;
    // Everything downstream of the video pass works in display orientation, so a
    // 90/270 degree rotation swaps the chain (and subtitle) dimensions.
    const bool rotOdd = (rotation_ & 1) != 0;
    const int dispW = rotOdd ? srcH_ : srcW_;
    const int dispH = rotOdd ? srcW_ : srcH_;
    const QSize wantSize(dispW, dispH);
    QSize contentPx;  // content rectangle in device pixels (whole pixels)
    bool scaled = false;
    {
        const QSize outPx = offscreen_
                                ? QSize()  // keep the last window layout: the preview is not read back
                                : (renderTarget() ? renderTarget()->pixelSize() : QSize());
        if (outPx.width() > 0 && outPx.height() > 0 && dispW > 0 && dispH > 0) {
            // Snap the content rectangle to whole pixels and derive the letterbox
            // scale from it, so a chain texture of exactly this size lands 1:1 on
            // the screen (no half-texel blur on subtitles). Keep the left/right
            // and top/bottom margins integral as well, otherwise the quad starts
            // at a half pixel and bilinear sampling smears the text.
            int cw = std::max(1, int(std::lround(double(outPx.width()) * double(scaleX_))));
            int ch = std::max(1, int(std::lround(double(outPx.height()) * double(scaleY_))));
            if (((outPx.width() - cw) & 1) != 0 && cw > 1) --cw;
            if (((outPx.height() - ch) & 1) != 0 && ch > 1) --ch;
            contentPx = QSize(cw, ch);
            scaleX_ = float(cw) / float(outPx.width());
            scaleY_ = float(ch) / float(outPx.height());
            const double sx = double(cw) / double(dispW);
            const double sy = double(ch) / double(dispH);
            scaled = std::fabs(sx - 1.0) > 0.01 || std::fabs(sy - 1.0) > 0.01;
            // One output pixel covers 1/sourceScale source texels; below 1x the
            // kernel widens to act as a low-pass.
            spacingX_ = float(std::max(1.0, 1.0 / std::max(sx, 1e-3)));
            spacingY_ = float(std::max(1.0, 1.0 / std::max(sy, 1e-3)));
        }
    }
    // Subtitles are rasterised at the size they end up on screen, not at video
    // resolution, so they stay sharp when the window is larger than the video.
    // While recording they are burned into the file instead, at video size.
    const bool wantRecord =
        (recorder_ && recorder_->running()) || capturePending_;
    const QSize subSize = wantRecord ? wantSize
                                     : ((scaled && contentPx.width() > 0) ? contentPx : wantSize);
    const bool wantScale = scalerMode_ > 0 && scaled;
    const bool chainActive = wantEffects || wantSubs || wantScale || wantRecord;
    // Zero-copy frames are display-referred sRGB already, so an SDR chain can
    // use them as they are; with scRGB output they still have to be linearised.
    // Uploads (YUV planes or the sws RGBA) always go through the colour
    // pipeline: the RGBA one still carries PQ/HLG. A display matrix also needs
    // the video pass: rotation must happen before anything filters or scales,
    // and the composite pass must not rotate the (already upright) subtitles.
    // Recording always needs it: the record pass copies the chain output, and
    // the readback wants plain RGBA8 in display encoding.
    const bool wantVideo =
        (chainActive && (!zc_ || hdrOutput_ || rotation_ != 0 || mirror_)) || wantRecord;
    const bool chainDirty =
        wantEffects != chainEffects_ || wantSubs != chainSubs_ || wantScale != chainScale_ ||
        wantRecord != chainRecord_ ||
        (chainActive &&
         (wantVideo != chainNeedVideo_ || wantSize != chainSize_ || hdrOutput_ != chainHdr_ ||
          // The scale pass target only matters when a composite follows it.
          (wantSubs && wantScale && subSize != chainOut_)));
    if (chainDirty) {
        chainEffects_ = wantEffects;
        chainSubs_ = wantSubs;
        chainScale_ = wantScale;
        chainRecord_ = wantRecord;
        chainNeedVideo_ = wantVideo;
        chainSize_ = wantSize;
        chainOut_ = subSize;
        chainHdr_ = hdrOutput_;
        rebuildChain();
    } else if (!chainActive) {
        chainHdr_ = hdrOutput_;  // nothing built yet; keep the change detector honest
    }

    // Rasterise the overlay when the cue or the output size changes. An empty
    // cue just switches the composite pass off, so no upload is needed at the
    // end of a line.
    if (chainSubs_ && haveFrame && !subText_.isEmpty()) {
        if (subText_ != renderedSubText_ || subTexSize_ != subSize) {
            subImage_ = renderSubtitleImage(subText_, subSize);
            if (!subTex_ || subTexSize_ != subSize) {
                subTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, subSize));
                if (subTex_->create()) {
                    subTexSize_ = subSize;
                } else {
                    LOGE("subtitle: cannot create the %dx%d overlay texture", subSize.width(),
                         subSize.height());
                    subTex_.reset();
                    subTexSize_ = QSize();
                }
            }
            renderedSubText_ = subText_;
            subDirty_ = subTex_ != nullptr;
        }
    } else if (subText_.isEmpty()) {
        renderedSubText_.clear();
    }

    if (haveFrame && (present_ || scalePass_ || compositePass_ || recordPass_)) {
        // Wire the chain front to back: each stage feeds the next. The order
        // follows rebuildChain(): the scale pass runs before the composite when
        // the subtitles are composited at output resolution, and after it when
        // the recording layout put the composite at video size.
        const bool rec = chainRecord_;
        const ColorParams cp = colorParamsFor(*frame_);
        // scRGB parameters: SDR white and the panel peak, both in "1.0 = 80
        // nits" units (see Hdr.h / video.frag).
        const float sdrScale = Hdr::sdrWhiteScale(hdrSdrWhite_);
        const float peakSc = Hdr::nitsToScRgb(hdrPeakNits_);
        QRhiTexture* base = curA_;
        bool baseRgba = rgba_;
        bool chained = false;  // true once any pass has rewritten the pixels
        if (videoPass_) {
            videoPass_->setSource(curA_, curB_, rgba_);
            videoPass_->setScale(1.f, 1.f);
            // The video pass is the one that turns the stored frame upright, so
            // everything after it (effect/subtitle/scale) works in display space.
            videoPass_->setRotation(rotation_, mirror_);
            ColorParams vp = cp;
            // 3 = display-referred sRGB (zero-copy): linearised for scRGB and
            // passed through unchanged for SDR. 2 = RGB video frame, i.e. the
            // sws upload, which still carries the source transfer function.
            vp.mode = rgba_ ? (zc_ ? 3 : 2) : 0;
            videoPass_->setColor(vp);
            videoPass_->setParams(sdrScale, peakSc, 0.f, 0.f);
            base = videoPass_->texture();
            baseRgba = true;
            chained = true;
        }
        if (effectPass_) {
            effectPass_->setSource(base, dummyTex_.get(), true);
            effectPass_->setScale(1.f, 1.f);
            effectPass_->setParams(saturation_, brightness_);
            // Only for color.w: the effect must not clamp linear highlights.
            effectPass_->setColor(cp);
            base = effectPass_->texture();
            baseRgba = true;
            chained = true;
        }
        if (scalePass_ && !rec) {
            // The chain guarantees display-ready RGB here (the video pass runs
            // whenever the source is YUV). With subtitles the resampler writes an
            // offscreen texture the size of the content rectangle (1:1, the
            // composite letterboxes it); without, it writes the swapchain itself.
            const bool toOutput = compositePass_ && chainOut_.width() > 0;
            scalePass_->setSource(base, dummyTex_.get(), true);
            scalePass_->setScale(toOutput ? 1.f : scaleX_, toOutput ? 1.f : scaleY_);
            scalePass_->setParams(spacingX_, spacingY_, float(scalerMode_), 0.f);
            base = scalePass_->texture();
            baseRgba = true;
            chained = true;
        }
        if (compositePass_) {
            compositePass_->setSource(base, subTex_ ? subTex_.get() : dummyTex_.get(), true);
            // Without recording this is the final pass: the vertex stage
            // letterboxes and both textures are sampled 1:1 (the overlay is
            // rasterised at exactly this size). While recording it writes the
            // video-sized offscreen texture instead, and the preview letterboxes
            // afterwards, so the file gets the same picture without the bars.
            compositePass_->setScale(rec ? 1.f : scaleX_, rec ? 1.f : scaleY_);
            // extra.z lifts the overlay to SDR white in scRGB output.
            compositePass_->setParams(subText_.isEmpty() ? 0.f : 1.f, 0.f,
                                      hdrOutput_ ? sdrScale : 1.f, 0.f);
            base = compositePass_->texture();
            baseRgba = true;
            chained = true;
        }
        if (recordPass_) {
            // HDR only: a branch, not part of the preview. It tone maps the chain
            // output into the RGBA8 texture the encoder reads back; base stays the
            // chain output, so the screen keeps the HDR encoding.
            recordPass_->setSource(base, dummyTex_.get(), true);
            recordPass_->setScale(1.f, 1.f);
            recordPass_->setRotation(0, false);
            recordPass_->setColor(cp);
            // extra.x = SDR white, extra.y = peak (scRGB units), extra.z = linear.
            recordPass_->setParams(sdrScale, peakSc, 1.f, 0.f);
            recordTex_ = recordPass_->texture();
        } else if (rec) {
            // SDR: the chain output is what the file gets, verbatim.
            recordTex_ = base;
        }
        if (scalePass_ && rec) {
            // Final pass of the recording layout: the resampler letterboxes the
            // (video sized) chain output into the swapchain.
            scalePass_->setSource(base, dummyTex_.get(), true);
            scalePass_->setScale(scaleX_, scaleY_);
            scalePass_->setParams(spacingX_, spacingY_, float(scalerMode_), 0.f);
            base = scalePass_->texture();
            baseRgba = true;
            chained = true;
        }
        if (present_) {
            present_->setSource(base, baseRgba ? dummyTex_.get() : curB_, baseRgba);
            present_->setScale(scaleX_, scaleY_);
            // No chain: this pass samples the video texture directly, so it is
            // the one that applies the display matrix. With a chain it only
            // copies the chain output, which is already upright.
            const bool fromVideo = !chained;
            present_->setRotation(fromVideo ? rotation_ : 0, fromVideo && mirror_);
            ColorParams d = cp;
            if (chained) {
                // The chain already produced pixels in the output encoding (sRGB
                // in SDR, linear scRGB in HDR). Copy them: running the source
                // colour conversion again would linearise them twice.
                d.mode = 1;
            } else if (baseRgba) {
                // 3 for zero-copy output (already display-referred sRGB), 2 for
                // the sws upload so PQ/HLG sources still get decoded.
                d.mode = zc_ ? 3 : 2;
            } else {
                // YUV planes: the present pass does the colour conversion itself.
                d.mode = 0;
            }
            present_->setColor(d);
            present_->setParams(sdrScale, peakSc, 0.f, 0.f);
        }
    }
    // Only the fallback path needs an upload; zero-copy writes straight to VRAM.
    pendingUpload_ = (haveFrame && !frame_->hw && frame_->frame != uploaded_) ? frame_->frame : nullptr;

    // One readback per newly presented frame: the picture the user is looking
    // at, tagged with its media timestamp so the file follows the video clock
    // (a 24 fps file on a 60 Hz screen is written as 24 fps).
    if (chainRecord_ && haveFrame && frame_ && frame_->frame
        && (frame_ != recordedFrame_ || capturePending_)) {
        recordedFrame_ = frame_;
        recordPts_ = frame_->pts;
        recordPending_ = true;
    } else if (!chainRecord_) {
        recordedFrame_.reset();
        recordPending_ = false;
        recordTex_ = nullptr;
    }

    if (!haveFrame) {
        // Nothing to show yet (or the decoder failed): clear and present black
        // so the item never keeps stale content.
        QRhiRenderTarget* t = offscreen_ ? offscreenRt_.get() : renderTarget();
        if (!t) return;
        cb->beginPass(t, QColor(0, 0, 0), { 1.0f, 0 });
        cb->endPass();
        return;
    }

    graph_->run(cb, offscreen_ ? offscreenRt_.get() : renderTarget());
}

bool VideoRenderer::renderOffscreen(QQuickRhiItem* item) {
    if (!rhi_ || rhi_->isDeviceLost()) return false;
    std::lock_guard<std::recursive_mutex> frameLock(frameMutex());

    if (!offscreenRt_) {
        // Stand-in for the swapchain. The preview passes write here (64x36 is
        // plenty: nothing reads them back); the chain keeps its own video-sized
        // targets, and the readback goes through the chain, not this.
        const QRhiTexture::Format fmt = hdrOutput_ ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
        offscreenTex_.reset(rhi_->newTexture(fmt, QSize(64, 36), 1, QRhiTexture::RenderTarget));
        if (!offscreenTex_->create()) {
            offscreenTex_.reset();
            return false;
        }
        offscreenRt_.reset(rhi_->newTextureRenderTarget({ QRhiColorAttachment(offscreenTex_.get()) }));
        if (!offscreenRt_->create()) {
            offscreenRt_.reset();
            return false;
        }
    }

    // Qt may be in the middle of a scene-graph frame on its render thread (it
    // says so and refuses), so wait a moment and retry. The caller keeps the
    // frame and tries again on the next tick: nothing is lost.
    QRhiCommandBuffer* cb = nullptr;
    QRhi::FrameOpResult r = rhi_->beginOffscreenFrame(&cb);
    for (int i = 0; i < 8 && (r != QRhi::FrameOpSuccess || !cb); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        r = rhi_->beginOffscreenFrame(&cb);
    }
    if (r != QRhi::FrameOpSuccess || !cb) return false;

    // Frames normally arrive through Qt's synchronize(); Qt does not sync a
    // hidden window, and during an export the caller picks the frames.
    synchronize(item);

    offscreen_ = true;
    render(cb);
    offscreen_ = false;
    rhi_->endOffscreenFrame();
    return true;
}

}  // namespace

VideoItem::VideoItem(QQuickItem* parent) : QQuickRhiItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
    // Hold the frame lock for Qt's whole scene-graph frame (the signals fire on
    // the render thread, hence the direct connections), so an offline export
    // frame from the GUI thread can never interleave with it.
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* w) {
        QObject::disconnect(frameLockBegin_);
        QObject::disconnect(frameLockEnd_);
        if (!w) return;
        frameLockBegin_ = QObject::connect(w, &QQuickWindow::beforeRendering, this,
                                           [] { frameMutex().lock(); }, Qt::DirectConnection);
        frameLockEnd_ = QObject::connect(w, &QQuickWindow::afterRendering, this,
                                         [] { frameMutex().unlock(); }, Qt::DirectConnection);
    });
    // Overridable from the environment so the effect path can be exercised
    // without UI: PLAY_SAT=1.4 PLAY_BRIGHT=0.05
    if (qEnvironmentVariableIsSet("PLAY_SAT")) saturation_ = qEnvironmentVariable("PLAY_SAT").toDouble();
    if (qEnvironmentVariableIsSet("PLAY_BRIGHT")) brightness_ = qEnvironmentVariable("PLAY_BRIGHT").toDouble();
    if (qEnvironmentVariableIsSet("PLAY_SCALER"))
        scalerMode_ = Scaler::fromName(qEnvironmentVariable("PLAY_SCALER"));

    // Recently played list and resume points. Tests and screenshots can point
    // it at a scratch file, or disable it entirely.
    if (qEnvironmentVariableIsSet("PLAY_NO_RESUME")) noResume_ = true;
    const QString libPath = qEnvironmentVariableIsSet("PLAY_LIBRARY")
                                ? qEnvironmentVariable("PLAY_LIBRARY")
                                : Library::defaultPath();
    library_.load(libPath);

    tick_.setInterval(16);
    connect(&tick_, &QTimer::timeout, this, [this] {
        // An offline export drives its own frames and returns before update():
        // Qt then has little reason to render between them, which keeps the
        // device free for our offscreen frames.
        if (tickOfflineExport()) return;
        update();
        // A minimised window is not rendered by Qt, which used to pause the
        // recording. Drive the same renderer offscreen while we are recording.
        // (The renderer serialises this against Qt's frames.)
        if (player_ && (recording_ || exporting()) && rendererHandle_ && window()
            && !window()->isExposed()) {
            if (auto* r = rendererHandle_->load())
                static_cast<VideoRenderer*>(r)->renderOffscreen(this);
        }
        if (player_) {
            // --export: start once the duration is known.
            if (pendingExport_ && exportStage_ == ExportStage::Idle && !recording_
                && duration() > 0.1) {
                pendingExport_ = false;
                exportStart_ = pendingExportA_;
                exportEnd_ = pendingExportB_;
                emit exportMarksChanged();
                exportClip();
            }
            // Clip export: wait for the seek to A to land, then record until B.
            if (exportStage_ == ExportStage::Seeking) {
                const qreal pos = player_->position();
                const qint64 waited = QDateTime::currentMSecsSinceEpoch() - exportSeekMs_;
                if (waited > 1500
                    || (pos >= exportStart_ - 0.05 && pos <= exportStart_ + 0.4)) {
                    const QString file = exportFile_;
                    if (startRecordingInternal(file)) {
                        exportStage_ = ExportStage::Recording;
                        // Cut exactly at B: the recorder drops everything past
                        // the limit, so the clip is B-A long even though the
                        // clock and the encoder lag a little.
                        player_->setRecordingLimit(exportEnd_ - exportStart_);
                    } else {
                        exportStage_ = ExportStage::Idle;
                        setPlaybackSpeed(exportResumeSpeed_);
                        exportFile_.clear();
                        emit exportingChanged();
                        emit exportFinished(file, false);
                    }
                }
            } else if (exportStage_ == ExportStage::Recording) {
                if (player_->recordingLimitReached()
                    || player_->position() >= exportEnd_ + 2.0 || player_->isFinished()) {
                    const QString file = exportFile_;
                    player_->pause();
                    stopRecording();
                    exportStage_ = ExportStage::Idle;
                    setPlaybackSpeed(exportResumeSpeed_);
                    exportFile_.clear();
                    emit exportingChanged();
                    emit exportFinished(file, true);
                }
            }
            const bool fin = player_->isFinished();
            if (fin != finished_) {
                finished_ = fin;
                if (fin) markWatched();
                emit finishedChanged();
                emit playingChanged();
                // The file ended: finish the recording too, so the last fragment
                // and the index are written while the user is still watching.
                if (fin && recording_) stopRecording();
            }
            // Persist the resume point every ~5 s of playback.
            if ((++saveTick_ % 300) == 0) persistPosition();
            // The encoder can stop on its own (write error, file closed): keep
            // the UI honest without polling from QML.
            const bool recNow = player_->isRecording();
            if (recNow != recording_) {
                recording_ = recNow;
                emit recordingChanged();
            }
        }
        if (++tickCount_ % 6 == 0) emit positionChanged();
    });

}

VideoItem::~VideoItem() {
    tick_.stop();
    persistPosition();
    if (player_) player_->close();
}

QQuickRhiItemRenderer* VideoItem::createRenderer() {
    auto* r = new VideoRenderer();
    if (!rendererHandle_) rendererHandle_ = std::make_shared<std::atomic<QQuickRhiItemRenderer*>>();
    r->setHandle(rendererHandle_);
    rendererHandle_->store(r);
    return r;
}

void VideoItem::setSource(const QString& s) {
    if (s == source_) return;
    open(s.isEmpty() ? QUrl() : QUrl::fromUserInput(s));
}

void VideoItem::open(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path != source_) {
        persistPosition();  // remember where we stopped in the previous file
        source_ = path;
        emit sourceChanged();
    }
    startPlayback(path);
}

void VideoItem::startPlayback(const QString& path) {
    if (!player_) player_ = std::make_unique<Player>();
    player_->close();
    // A/B marks belong to the file that was open: drop them with it, and abort
    // an export that was running on the old file.
    if (exporting()) cancelExport();
    if (exportStart_ != 0.0 || exportEnd_ != 0.0) {
        exportStart_ = 0.0;
        exportEnd_ = 0.0;
        emit exportMarksChanged();
    }
    // The rate can be set before the first file (--speed, restored settings);
    // apply it now so open() builds the decoder at the right speed.
    if (playbackSpeed_ != 1.0) player_->setSpeed(playbackSpeed_);
    player_->setVolume(static_cast<float>(volume_));
    tick_.stop();
    error_.clear();
    emit errorStringChanged();
    if (finished_) {
        finished_ = false;
        emit finishedChanged();
    }
    emit audioTracksChanged();

    subs_.clear();
    subFiles_.clear();
    embeddedSubs_.clear();
    embeddedList_.clear();
    subtitlesEnabled_ = false;

    if (path.isEmpty()) {
        emit subtitlesChanged();
        update();
        return;
    }

    std::string err;
    if (!player_->open(path.toStdString(), &err)) {
        setError(QString::fromStdString(err));
        update();
        return;
    }

    // Zero-copy is the default: the video processor runs on the decoder's
    // device and the result reaches the RHI through a shared texture. Drivers
    // without shared NT-handle / fence support fall back automatically;
    // PLAY_NO_ZEROCOPY=1 forces the CPU upload path (used for A/B benchmarks).
    if (qEnvironmentVariableIsSet("PLAY_NO_ZEROCOPY")) player_->setHardwarePassthrough(false);
    player_->setLoop(loop_);
    saveTick_ = 0;

    // Recent list + resume. The position is read before touch() so a file that
    // is opened again keeps its bookmark.
    if (!noResume_) {
        const double resume = library_.positionFor(path);
        library_.touch(path, player_->duration());
        emit recentChanged();
        if (Library::shouldResume(resume, player_->duration())) {
            player_->seek(resume);
            LOGI("resume: %s at %.1fs", qPrintable(QFileInfo(path).fileName()), resume);
        }
    }

    // Sidecar subtitles: "movie.srt", then "movie.zh.srt" and friends. The
    // Chinese tag wins when several languages are present.
    subFiles_ = findSidecarSubtitles(path);
    if (!subFiles_.isEmpty()) {
        QString pick = subFiles_.first();
        for (const QString& f : subFiles_) {
            const QString l = f.toLower();
            if (l.contains(QStringLiteral(".zh")) || l.contains(QStringLiteral(".chs")) ||
                l.contains(QStringLiteral(".chi")) || l.contains(QStringLiteral(".sc.")) ||
                l.contains(QStringLiteral(".gb"))) {
                pick = f;
                break;
            }
        }
        QString suberr;
        if (subs_.loadFile(pick, &suberr)) {
            subtitlesEnabled_ = true;
        } else {
            LOGW("subtitle: %s", qPrintable(suberr));
        }
    }

    // Embedded subtitle streams: list them for the menu, and when no sidecar
    // was found pick the Chinese track if there is one, otherwise the only
    // text track. Bitmap formats (PGS/DVD) are listed but not selectable.
    {
        embeddedSubs_ = SubtitleTrack::embeddedTracks(path);
        int firstText = -1;
        int textCount = 0;
        int zhText = -1;
        for (const auto& t : embeddedSubs_) {
            QVariantMap m;
            m.insert(QStringLiteral("index"), t.index);
            const QString lang = t.language.toLower();
            const bool zh = lang.startsWith(QStringLiteral("zh")) ||
                            lang.startsWith(QStringLiteral("chi")) ||
                            lang == QLatin1String("zho");
            const QString name = !t.title.isEmpty()
                                     ? t.title
                                     : (t.language.isEmpty() ? QStringLiteral("未知语言")
                                                             : t.language);
            m.insert(QStringLiteral("label"),
                     QStringLiteral("内嵌 %1 · %2%3").arg(
                         name, t.codec, t.text ? QString() : QStringLiteral("（位图）")));
            m.insert(QStringLiteral("codec"), t.codec);
            m.insert(QStringLiteral("language"), t.language);
            m.insert(QStringLiteral("text"), t.text);
            embeddedList_ << m;

            if (t.text) {
                ++textCount;
                if (firstText < 0) firstText = t.index;
                if (zh && zhText < 0) zhText = t.index;
            }
        }
        if (subs_.empty() && textCount > 0) {
            const int autoPick = zhText >= 0 ? zhText : (textCount == 1 ? firstText : -1);
            if (autoPick >= 0) selectEmbeddedSubtitle(autoPick);
        }
    }
    emit subtitlesChanged();

    tick_.start();
    emit durationChanged();
    emit playingChanged();
    emit sourceChanged();  // refresh the decoder badge
    emit audioTracksChanged();
    update();
}

void VideoItem::setError(const QString& e) {
    error_ = e;
    emit errorStringChanged();
}

bool VideoItem::playing() const {
    return player_ && !player_->isPaused() && !finished_ && !source_.isEmpty();
}

bool VideoItem::isFinished() const { return finished_; }

bool VideoItem::isLooping() const { return loop_; }

void VideoItem::setLoop(bool on) {
    if (loop_ == on) return;
    loop_ = on;
    if (player_) player_->setLoop(on);
    emit loopChanged();
}

qreal VideoItem::position() const { return player_ ? player_->position() : 0.0; }
qreal VideoItem::duration() const { return player_ ? player_->duration() : 0.0; }
bool VideoItem::hardwareDecoding() const { return player_ && player_->hardwareDecoding(); }

QVariantList VideoItem::audioTracks() const {
    QVariantList out;
    if (!player_) return out;
    const auto tracks = player_->audioTracks();
    const int current = player_->currentAudioTrack();
    for (const auto& t : tracks) {
        QVariantMap m;
        m.insert(QStringLiteral("index"), t.streamIndex);
        QString label = t.title.empty() ? QString() : QString::fromStdString(t.title);
        if (label.isEmpty() && !t.language.empty())
            label = QString::fromStdString(t.language);
        if (label.isEmpty()) label = QStringLiteral("音轨 %1").arg(out.size() + 1);
        if (t.channels > 0) label += QStringLiteral(" (%1ch)").arg(t.channels);
        m.insert(QStringLiteral("label"), label);
        m.insert(QStringLiteral("current"), t.streamIndex == current);
        out.push_back(m);
    }
    return out;
}

void VideoItem::selectAudioTrack(int streamIndex) {
    if (!player_) return;
    player_->selectAudioTrack(streamIndex);
    emit audioTracksChanged();
}

void VideoItem::setSaturation(qreal v) {
    if (qFuzzyCompare(saturation_, v)) return;
    saturation_ = v;
    emit saturationChanged();
    update();
}

void VideoItem::setBrightness(qreal v) {
    if (qFuzzyCompare(brightness_, v)) return;
    brightness_ = v;
    emit brightnessChanged();
    update();
}

void VideoItem::setScaler(const QString& name) {
    const int mode = Scaler::fromName(name);
    if (mode == scalerMode_) return;
    scalerMode_ = mode;
    emit scalerChanged();
    update();
}

void VideoItem::setHdr(bool on) {
    if (on == hdr_) return;
    hdr_ = on;
    // scRGB is linear with values above 1.0; an 8-bit buffer would clip every
    // highlight before the compositor ever sees it.
    setColorBufferFormat(on ? TextureFormat::RGBA16F : TextureFormat::RGBA8);
    emit hdrChanged();
    update();
}

void VideoItem::setPlaybackSpeed(qreal v) {
    v = std::clamp(v, 0.25, 4.0);
    if (qFuzzyCompare(playbackSpeed_, v)) return;
    playbackSpeed_ = v;
    if (player_) player_->setSpeed(v);
    emit playbackSpeedChanged();
    emit positionChanged();
    update();
}

void VideoItem::setVolume(qreal v) {
    v = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(volume_ + 0.0001, v + 0.0001)) return;
    if (v > 0.0001) volumeBeforeMute_ = v;
    volume_ = v;
    if (player_) player_->setVolume(static_cast<float>(v));
    emit volumeChanged();
}

void VideoItem::toggleMute() { setVolume(muted() ? volumeBeforeMute_ : 0.0); }

QStringList VideoItem::findSidecarSubtitles(const QString& path) {
    const QFileInfo fi(path);
    const QDir dir(fi.absolutePath());
    const QString base = fi.completeBaseName();
    QStringList out;

    static const QStringList exts = { QStringLiteral("srt"), QStringLiteral("ass"),
                                      QStringLiteral("ssa"), QStringLiteral("vtt") };
    for (const QString& e : exts) {
        const QString p = dir.absoluteFilePath(base + QLatin1Char('.') + e);
        if (QFileInfo::exists(p)) out << p;
    }
    for (const QString& e : exts) {
        const QStringList named =
            dir.entryList({ base + QStringLiteral(".*.") + e }, QDir::Files, QDir::Name);
        for (const QString& n : named) out << dir.absoluteFilePath(n);
    }
    out.removeDuplicates();
    return out;
}

void VideoItem::setSubtitlesEnabled(bool on) {
    if (subtitlesEnabled_ == on) return;
    subtitlesEnabled_ = on;
    emit subtitlesChanged();
    update();
}

void VideoItem::toggleSubtitles() {
    if (subs_.empty()) return;
    setSubtitlesEnabled(!subtitlesEnabled_);
}

bool VideoItem::startRecording(const QString& url) {
    if (exporting()) return false;  // the export owns the recorder
    return startRecordingInternal(url);
}

bool VideoItem::startRecordingInternal(const QString& url) {
    if (recording_) return true;
    if (!player_ || !player_->hasVideo()) {
        setError(QStringLiteral("nothing to record: no video stream"));
        return false;
    }
    QString path;
    if (!url.isEmpty()) {
        path = url;
    } else {
        // Next to the file being played: that is where the user will look for it.
        const QFileInfo fi(source_);
        QString base = fi.completeBaseName();
        if (base.isEmpty()) base = QStringLiteral("recording");
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        path = QDir(fi.absolutePath())
                   .filePath(QStringLiteral("%1_%2.mp4").arg(base, stamp));
    }
    std::string err;
    if (!player_->startRecording(path.toStdString(), &err)) {
        setError(QString::fromStdString(err));
        return false;
    }
    recording_ = true;
    recordingFile_ = path;
    emit recordingChanged();
    return true;
}

void VideoItem::stopRecording() {
    if (!player_) return;
    player_->stopRecording();
    if (recording_) {
        recording_ = false;
        emit recordingChanged();
    }
}

void VideoItem::toggleRecording() {
    // During a clip export the recorder belongs to the export: Ctrl+R must not
    // cut it short (Ctrl+E cancels it).
    if (exporting()) return;
    if (recording_) {
        stopRecording();
    } else {
        startRecording(QString());
    }
}

void VideoItem::markExportStart(qreal seconds) {
    if (!player_) return;
    const qreal total = duration();
    const qreal t = seconds >= 0 ? seconds : player_->position();
    exportStart_ = qBound<qreal>(0.0, t, total > 0 ? total : t);
    // A mark past B would make the range empty: drop the other mark instead.
    if (exportEnd_ > 0 && exportEnd_ <= exportStart_ + 0.05) exportEnd_ = 0;
    emit exportMarksChanged();
}

void VideoItem::markExportEnd(qreal seconds) {
    if (!player_) return;
    const qreal total = duration();
    const qreal t = seconds >= 0 ? seconds : player_->position();
    exportEnd_ = qBound<qreal>(0.0, t, total > 0 ? total : t);
    if (exportEnd_ <= exportStart_ + 0.05) exportStart_ = 0;
    emit exportMarksChanged();
}

bool VideoItem::exportClip() {
    if (!player_) return false;
    if (exporting()) return false;
    if (recording_ || !player_->hasVideo() || duration() <= 0.1) {
        setError(QStringLiteral("export: nothing to export"));
        return false;
    }
    qreal a = exportStart_;
    qreal b = exportEnd_ > 0 ? exportEnd_ : duration();
    a = qBound<qreal>(0.0, a, duration());
    b = qBound<qreal>(0.0, b, duration());
    if (b - a < 0.2) {
        setError(QStringLiteral("export: mark A and B first (the range is empty)"));
        return false;
    }
    const QFileInfo fi(source_);
    QString base = fi.completeBaseName();
    if (base.isEmpty()) base = QStringLiteral("clip");
    auto stamp = [](qreal t) {
        const int secs = int(t + 0.5);
        return QStringLiteral("%1m%2s")
            .arg(secs / 60, 2, 10, QLatin1Char('0'))
            .arg(secs % 60, 2, 10, QLatin1Char('0'));
    };
    exportFile_ = QDir(fi.absolutePath())
                      .filePath(QStringLiteral("%1_clip_%2-%3.mp4")
                                    .arg(base, stamp(a), stamp(b)));
    exportStart_ = a;
    exportEnd_ = b;
    emit exportMarksChanged();
    exportResumeSpeed_ = playbackSpeed_;
    setPlaybackSpeed(1.0);
    // Offline by default (much faster); PLAY_EXPORT_REALTIME=1 keeps the old
    // behaviour, which records the playback exactly as it happens on screen.
    offlineExport_ = !qEnvironmentVariableIsSet("PLAY_EXPORT_REALTIME");
    exportProgress_ = 0.0;
    emit exportProgressChanged();
    exportStage_ = ExportStage::Seeking;
    exportSeekMs_ = QDateTime::currentMSecsSinceEpoch();
    emit exportingChanged();
    if (player_->isPaused()) player_->play();
    seekTo(a);
    return true;
}

void VideoItem::exportWhenReady(qreal a, qreal b) {
    pendingExport_ = true;
    pendingExportA_ = a;
    pendingExportB_ = b;
}

bool VideoItem::tickOfflineExport() {
    if (!player_ || !offlineExport_ || exportStage_ == ExportStage::Idle) return false;
    if (!player_->offline()) player_->setOffline(true);
    auto* rr = rendererHandle_ ? rendererHandle_->load() : nullptr;
    // A few frames per tick: the UI (cancel, progress) stays responsive even
    // while the export runs as fast as the pipeline allows.
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 40;
    bool done = !rr;
    bool ok = false;
    for (;;) {
        if (!rr) break;
        if (!pendingOfflineFrame_) {
            VideoFramePtr f;
            if (!player_->pullFrame(f)) {
                // End of file (or the decoder stalled): keep what we got.
                done = true;
                ok = recording_;
                break;
            }
            if (f->pts + 0.002 < exportStart_) continue;  // seek pre-roll
            if (exportStage_ == ExportStage::Seeking) {
                // The seek runs on the decode thread: until it lands the queue
                // still holds frames from where the player was. Only start on a
                // frame at A (a stale one would set the wrong timeline).
                if (f->pts > exportStart_ + 0.5) continue;
                if (f->pts > exportEnd_) {
                    done = true;  // A sits at/after the end of the file
                    ok = false;
                    break;
                }
                // Open the encoder on the first frame we actually want, so the
                // pre-roll never lands in the file.
                if (!startRecordingInternal(exportFile_)) {
                    done = true;
                    ok = false;
                    break;
                }
                player_->setRecordingLimit(exportEnd_ - exportStart_);
                if (auto rec = player_->recorder()) rec->setBackpressure(true);
                exportStage_ = ExportStage::Recording;
                emit exportingChanged();
            }
            if (f->pts > exportEnd_ + 0.002) {
                done = true;
                ok = true;
                break;
            }
            pendingOfflineFrame_ = f;
        }
        // The preview renders this one until the next frame replaces it, so Qt
        // has something to show between our frames.
        offlineFrame_ = pendingOfflineFrame_;
        // Qt may be mid-frame on its render thread: keep the frame and retry on
        // the next tick instead of losing it.
        if (!static_cast<VideoRenderer*>(rr)->renderOffscreen(this)) break;
        const qreal pts = pendingOfflineFrame_->pts;
        pendingOfflineFrame_.reset();
        if (exportEnd_ > exportStart_) {
            const qreal p =
                qBound<qreal>(0.0, (pts - exportStart_) / (exportEnd_ - exportStart_), 1.0);
            if (p - exportProgress_ > 0.01) {
                exportProgress_ = p;
                emit exportProgressChanged();
            }
        }
        if (QDateTime::currentMSecsSinceEpoch() >= deadline) break;
    }
    if (done) finishExport(ok);
    return true;
}

void VideoItem::finishExport(bool ok) {
    const QString file = exportFile_;
    const qreal end = exportEnd_;
    if (recording_) stopRecording();
    if (player_) player_->setOffline(false);
    offlineExport_ = false;
    offlineFrame_.reset();
    exportStage_ = ExportStage::Idle;
    exportFile_.clear();
    exportProgress_ = 0.0;
    emit exportProgressChanged();
    setPlaybackSpeed(exportResumeSpeed_);
    if (player_ && end > 0.0) {
        // The clock means nothing after an offline burst: re-anchor it at the
        // end of the clip and stay paused there.
        player_->seek(end);
        player_->pause();
    }
    emit exportingChanged();
    emit exportFinished(file, ok);
}

void VideoItem::cancelExport() {
    if (!exporting()) return;
    const QString file = exportFile_;
    if (recording_) {
        player_->stopRecording();
        recording_ = false;
        recordingFile_.clear();
    }
    if (player_) player_->setOffline(false);
    offlineExport_ = false;
    offlineFrame_.reset();
    if (!file.isEmpty()) QFile::remove(file);  // keep the folder clean
    exportStage_ = ExportStage::Idle;
    exportFile_.clear();
    exportProgress_ = 0.0;
    setPlaybackSpeed(exportResumeSpeed_);
    emit recordingChanged();
    emit exportProgressChanged();
    emit exportingChanged();
    emit exportFinished(file, false);
}

QString VideoItem::takeScreenshotRequest() {
    QString p;
    p.swap(shotRequest_);
    return p;
}

bool VideoItem::saveScreenshot(const QString& url) {
    if (!player_ || !player_->hasVideo()) {
        setError(QStringLiteral("nothing to capture: no video stream"));
        return false;
    }
    QString path;
    if (!url.isEmpty()) {
        path = url;
    } else {
        const QFileInfo fi(source_);
        QString base = fi.completeBaseName();
        if (base.isEmpty()) base = QStringLiteral("frame");
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        path = QDir(fi.absolutePath())
                   .filePath(QStringLiteral("%1_%2.png").arg(base, stamp));
    }
    shotRequest_ = path;
    LOGI("shot: requested %s", qPrintable(path));
    update();
    return true;
}

QString VideoItem::subtitleLabel() const {
    return subs_.label();
}

bool VideoItem::hasSubtitles() const { return !subs_.empty(); }

QString VideoItem::subtitleTextAt(double t) const {
    if (!subtitlesEnabled_) return {};
    return subs_.textAt(t);
}

bool VideoItem::openSubtitle(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QString err;
    if (!subs_.loadFile(path, &err)) {
        setError(err);
        return false;
    }
    if (!subFiles_.contains(path)) subFiles_ << path;
    subtitlesEnabled_ = true;
    error_.clear();
    emit errorStringChanged();
    emit subtitlesChanged();
    update();
    return true;
}

void VideoItem::persistPosition() {
    if (noResume_ || source_.isEmpty() || !player_) return;
    const double dur = player_->duration();
    if (dur <= 0.0) return;
    const double pos = finished_ ? 0.0 : player_->position();
    if (source_ == lastPersistPath_ && std::fabs(pos - lastPersistPos_) < 0.05) return;
    lastPersistPath_ = source_;
    lastPersistPos_ = pos;
    library_.setPosition(source_, pos, dur);
    QString err;
    if (!library_.save(&err)) LOGW("library: save failed: %s", qPrintable(err));
}

void VideoItem::markWatched() {
    if (noResume_ || source_.isEmpty()) return;
    const double dur = player_ ? player_->duration() : 0.0;
    library_.setPosition(source_, 0.0, dur);
    lastPersistPath_ = source_;
    lastPersistPos_ = 0.0;
    QString err;
    if (!library_.save(&err)) LOGW("library: save failed: %s", qPrintable(err));
}

void VideoItem::forgetRecent(const QString& path) {
    library_.remove(path);
    QString err;
    if (!library_.save(&err)) LOGW("library: save failed: %s", qPrintable(err));
    if (path == source_) {
        lastPersistPath_.clear();
        lastPersistPos_ = -1.0;
    }
    emit recentChanged();
}

void VideoItem::clearRecent() {
    library_.clear();
    QString err;
    if (!library_.save(&err)) LOGW("library: save failed: %s", qPrintable(err));
    lastPersistPath_.clear();
    lastPersistPos_ = -1.0;
    emit recentChanged();
}

bool VideoItem::selectEmbeddedSubtitle(int streamIndex) {
    if (source_.isEmpty()) return false;
    QString err;
    if (!subs_.loadEmbedded(source_, streamIndex, &err)) {
        setError(err);
        emit subtitlesChanged();
        return false;
    }
    subtitlesEnabled_ = true;
    error_.clear();
    emit errorStringChanged();
    emit subtitlesChanged();
    update();
    return true;
}

void VideoItem::clearSubtitle() {
    subs_.clear();
    subtitlesEnabled_ = false;
    emit subtitlesChanged();
    update();
}

void VideoItem::togglePlay() {
    if (!player_) return;
    // After the last frame the button restarts from the beginning.
    if (finished_) {
        finished_ = false;
        emit finishedChanged();
        player_->seek(0.0);
        player_->play();
    } else {
        player_->togglePause();
    }
    emit playingChanged();
    emit audioTracksChanged();
    update();
}

void VideoItem::seekTo(qreal seconds) {
    if (!player_) return;
    player_->seek(seconds);
    emit positionChanged();
    update();
}
