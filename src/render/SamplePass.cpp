#include "SamplePass.h"

#include <QColor>

#include "core/Log.h"

SamplePass::SamplePass(RenderGraph* graph, const char* name, const QString& frag, QSize offscreen,
                       QRhiTexture::Format format)
    : g_(graph), name_(name), fragPath_(frag), size_(offscreen), format_(format) {}

void SamplePass::setSource(QRhiTexture* a, QRhiTexture* b, bool rgba) {
    srcA_ = a;
    srcB_ = b;
    rgba_ = rgba;
    color_.mode = rgba ? 1 : 0;
}

void SamplePass::setScale(float x, float y) {
    sx_ = x;
    sy_ = y;
}

void SamplePass::setParams(float x, float y, float z, float w) {
    params_[0] = x;
    params_[1] = y;
    params_[2] = z;
    params_[3] = w;
}

bool SamplePass::ensureTarget() {
    if (size_.isEmpty()) return true;  // swapchain
    if (tex_ && tex_->pixelSize() == size_ && tex_->format() == format_) return true;

    release();
    // Note: the sample count must be passed explicitly, otherwise the Flag
    // converts to int and is taken as the sample count.
    tex_.reset(g_->rhi()->newTexture(format_, size_, 1, QRhiTexture::RenderTarget));
    if (!tex_->create()) {
        LOGE("pass %s: cannot create %dx%d target (format %d)", name_, size_.width(),
             size_.height(), int(format_));
        tex_.reset();
        return false;
    }
    QRhiColorAttachment att(tex_.get());
    rt_.reset(g_->rhi()->newTextureRenderTarget({ att }));
    rpd_.reset(rt_->newCompatibleRenderPassDescriptor());
    rt_->setRenderPassDescriptor(rpd_.get());
    if (!rt_->create()) {
        LOGE("pass %s: cannot create render target", name_);
        release();
        return false;
    }
    return true;
}

QRhiShaderResourceBindings* SamplePass::bindingsFor(QRhiTexture* a, QRhiTexture* b) {
    for (const auto& entry : bindings_) {
        if (entry.a == a && entry.b == b) return entry.srb.get();
    }
    Binding entry;
    entry.a = a;
    entry.b = b;
    entry.srb.reset(g_->rhi()->newShaderResourceBindings());
    entry.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            ubuf_.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, a,
                                                  g_->sampler()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, b,
                                                  g_->sampler()),
    });
    entry.srb->create();
    bindings_.push_back(std::move(entry));
    return bindings_.back().srb.get();
}

bool SamplePass::prepare(QRhiRenderTarget* swapchain) {
    if (!g_ || !g_->ready()) return false;
    if (!srcA_ || !srcB_) return false;

    if (!ensureTarget()) return false;

    if (!ubuf_) {
        ubuf_.reset(g_->rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
        ubuf_->create();
    }

    const QRhiRenderPassDescriptor* rpdRaw =
        size_.isEmpty() ? swapchain->renderPassDescriptor() : rpd_.get();
    if (!rpdRaw) return false;
    auto* rpd = const_cast<QRhiRenderPassDescriptor*>(rpdRaw);

    curRt_ = size_.isEmpty() ? swapchain : rt_.get();
    cur_ = bindingsFor(srcA_, srcB_);

    if (!pipe_ || pipeRpd_ != rpd) {
        // A new swapchain descriptor (resize) invalidates the pipeline.
        QShader vs = loadShaderResource(QStringLiteral(":/shaders/video.vert.qsb"));
        QShader fs = loadShaderResource(fragPath_);
        if (!vs.isValid() || !fs.isValid()) return false;

        pipe_.reset(g_->rhi()->newGraphicsPipeline());
        pipe_->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs },
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({ { 4 * sizeof(float) } });
        layout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
        });
        pipe_->setVertexInputLayout(layout);
        pipe_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipe_->setShaderResourceBindings(cur_);
        pipe_->setRenderPassDescriptor(rpd);
        if (!pipe_->create()) {
            LOGE("pass %s: pipeline creation failed", name_);
            pipe_.reset();
            return false;
        }
        pipeRpd_ = rpd;
    }
    return true;
}

void SamplePass::update(QRhiResourceUpdateBatch* rub) {
    struct {
        float scale[4];   // x/y: letterbox scale, z: rotation (quarter turns), w: mirror
        float params[4];  // x: 0 = YUV, 1 = RGB passthrough, 2 = RGB video; y: matrix; z: full range; w: trc
        float extra[4];   // effect-specific parameters
        float color[4];   // x: primaries, y: peak nits, z: tone map, w: scRGB output
    } u{};
    u.scale[0] = sx_;
    u.scale[1] = sy_;
    u.scale[2] = float(rot_);          // display matrix: clockwise quarter turns
    u.scale[3] = mirror_ ? 1.f : 0.f;  // horizontal mirror
    u.params[0] = float(color_.mode);
    u.params[1] = float(color_.matrix);
    u.params[2] = color_.fullRange ? 1.f : 0.f;
    u.params[3] = float(color_.trc);
    u.extra[0] = params_[0];
    u.extra[1] = params_[1];
    u.extra[2] = params_[2];
    u.extra[3] = params_[3];
    u.color[0] = float(color_.primaries);
    u.color[1] = color_.peakNits;
    u.color[2] = color_.trc != 0 ? 1.f : 0.f;
    u.color[3] = color_.hdrOut ? 1.f : 0.f;
    rub->updateDynamicBuffer(ubuf_.get(), 0, sizeof(u), &u);
}

void SamplePass::draw(QRhiCommandBuffer* cb) {
    if (!cur_ || !pipe_ || !curRt_) return;
    const QSize px = curRt_->pixelSize();
    cb->setViewport({ 0.f, 0.f, static_cast<float>(px.width()),
                      static_cast<float>(px.height()) });
    cb->setGraphicsPipeline(pipe_.get());
    cb->setShaderResources(cur_);
    const QRhiCommandBuffer::VertexInput vi[] = { { g_->quad(), 0 } };
    cb->setVertexInput(0, 1, vi);
    cb->draw(4);
}

void SamplePass::release() {
    bindings_.clear();
    cur_ = nullptr;
    curRt_ = nullptr;
    pipe_.reset();
    pipeRpd_ = nullptr;
    ubuf_.reset();
    rpd_.reset();
    rt_.reset();
    tex_.reset();
}
