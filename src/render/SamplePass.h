#pragma once
#include <QSize>
#include <QString>
#include <memory>
#include <vector>

#include <rhi/qrhi.h>

#include "RenderGraph.h"

// Colour description handed to video.frag. The shader uses it to build the
// YUV->RGB matrix, undo the transfer function of HDR sources, tone map and
// convert BT.2020 to BT.709.
struct ColorParams {
    int mode = 0;        // 0 = YUV planes, 1 = RGB in target encoding, 2 = RGB video
                         // frame, 3 = RGB display-referred sRGB (zero-copy/VP output)
    int matrix = 1;      // 0 = BT.601, 1 = BT.709, 2 = BT.2020 NCL
    int trc = 0;         // 0 = SDR, 1 = PQ, 2 = HLG
    int primaries = 0;   // 0 = BT.709/sRGB, 1 = BT.2020
    bool fullRange = false;
    float peakNits = 100.f;
    // True when the swapchain is scRGB (FP16 linear) instead of SDR: the shader
    // then writes linear values with 1.0 = 80 nits and skips the sRGB encode.
    bool hdrOut = false;
};

// Full-screen pass that samples one or two textures with a fragment shader from
// the resource bundle and writes either an offscreen RGBA8 texture (effects,
// subtitles, ...) or the swapchain.
//
// The vertex stage is always shaders/video.vert: a full-screen quad scaled by
// ubuf.scale.xy (used for letterboxing at present time).
class SamplePass : public RenderPass {
public:
    // |format| is the offscreen target format; the HDR chain uses RGBA16F so
    // values above 1.0 (scRGB) survive between passes.
    SamplePass(RenderGraph* graph, const char* name, const QString& frag,
               QSize offscreen = QSize(), QRhiTexture::Format format = QRhiTexture::RGBA8);
    ~SamplePass() override { release(); }

    // |rgba| selects the RGBA path in video.frag (ignored by other shaders).
    void setSource(QRhiTexture* a, QRhiTexture* b, bool rgba);
    void setScale(float x, float y);
    // Display-matrix transform applied to the sampled texture: clockwise quarter
    // turns (0..3) and an optional horizontal mirror. Only the pass that samples
    // the *video* texture sets these; chain passes work in display orientation.
    void setRotation(int quarterTurns, bool mirror) {
        rot_ = quarterTurns & 3;
        mirror_ = mirror;
    }
    // Shader-specific parameters, handed over as ubuf.params / ubuf.extra.
    void setParams(float x, float y = 0.f, float z = 0.f, float w = 0.f);
    // Colour description for the video/present passes (see ColorParams).
    void setColor(const ColorParams& c) { color_ = c; }

    QRhiTexture* texture() const { return tex_.get(); }
    QSize offscreenSize() const { return size_; }

    const char* name() const override { return name_; }
    bool prepare(QRhiRenderTarget* swapchain) override;
    QRhiRenderTarget* renderTarget(QRhiRenderTarget* swapchain) override {
        return rt_ ? rt_.get() : swapchain;
    }
    void update(QRhiResourceUpdateBatch* rub) override;
    void draw(QRhiCommandBuffer* cb) override;
    void release() override;

private:
    QRhiShaderResourceBindings* bindingsFor(QRhiTexture* a, QRhiTexture* b);
    bool ensureTarget();

    RenderGraph* g_ = nullptr;
    const char* name_ = "";
    QString fragPath_;
    QSize size_;  // offscreen size; empty means "draw into the swapchain"
    QRhiTexture::Format format_ = QRhiTexture::RGBA8;

    QRhiTexture* srcA_ = nullptr;
    QRhiTexture* srcB_ = nullptr;
    bool rgba_ = false;
    float sx_ = 1.f, sy_ = 1.f;
    int rot_ = 0;
    bool mirror_ = false;
    float params_[4] = { 0.f, 0.f, 0.f, 0.f };
    ColorParams color_;

    std::unique_ptr<QRhiTexture> tex_;
    std::unique_ptr<QRhiTextureRenderTarget> rt_;
    std::unique_ptr<QRhiRenderPassDescriptor> rpd_;
    std::unique_ptr<QRhiBuffer> ubuf_;
    std::unique_ptr<QRhiGraphicsPipeline> pipe_;
    QRhiRenderPassDescriptor* pipeRpd_ = nullptr;
    QRhiRenderTarget* curRt_ = nullptr;

    // The zero-copy path alternates between two imported textures, so the
    // bindings are cached per source pair instead of being rebuilt per frame.
    struct Binding {
        QRhiTexture* a;
        QRhiTexture* b;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
    };
    std::vector<Binding> bindings_;
    QRhiShaderResourceBindings* cur_ = nullptr;
};
