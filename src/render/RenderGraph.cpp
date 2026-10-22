#include "RenderGraph.h"

#include <QColor>
#include <QFile>

#include "core/Log.h"

namespace {
// Full-screen triangle strip: position (xz plane) + uv.
constexpr float kQuadVerts[] = {
    -1.f, -1.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,
     1.f,  1.f, 1.f, 0.f,
};
}  // namespace

QShader loadShaderResource(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        LOGE("cannot open shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

RenderGraph::~RenderGraph() { release(); }

void RenderGraph::init(QRhi* rhi) {
    if (rhi_ == rhi && quad_) return;
    release();
    rhi_ = rhi;

    quad_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                sizeof(kQuadVerts)));
    quad_->create();
    sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler_->create();
    quadUploaded_ = false;
}

void RenderGraph::release() {
    for (auto& p : passes_) p->release();
    passes_.clear();
    quad_.reset();
    sampler_.reset();
    quadUploaded_ = false;
    rhi_ = nullptr;
}

void RenderGraph::clearPasses() {
    for (auto& p : passes_) p->release();
    passes_.clear();
}

void RenderGraph::run(QRhiCommandBuffer* cb, QRhiRenderTarget* swapchain) {
    if (!rhi_ || !swapchain) return;

    bool first = true;
    for (auto& p : passes_) {
        if (!p->prepare(swapchain)) continue;

        QRhiResourceUpdateBatch* rub = rhi_->nextResourceUpdateBatch();
        if (!quadUploaded_) {
            // The vertex data is static; the first batch that is actually
            // submitted also carries it.
            rub->uploadStaticBuffer(quad_.get(), kQuadVerts);
            quadUploaded_ = true;
        }
        if (first && frameUpdate_) frameUpdate_(rub);
        first = false;

        p->update(rub);
        QRhiRenderTarget* rt = p->renderTarget(swapchain);
        cb->beginPass(rt, QColor(0, 0, 0), { 1.0f, 0 }, rub);
        p->draw(cb);
        cb->endPass();
    }
    // If every pass bailed out (no frame yet) the batch is dropped, which is
    // fine: nothing was uploaded that we cannot redo next frame.

    // Readbacks run after the passes so they see this frame's pixels.
    if (postUpdate_ && !passes_.empty()) {
        QRhiResourceUpdateBatch* rub = rhi_->nextResourceUpdateBatch();
        postUpdate_(rub);
        cb->resourceUpdate(rub);
    }
}
