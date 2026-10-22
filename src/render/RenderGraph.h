#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

#include <rhi/qrhi.h>

// Loads a Qt-shader (.qsb) from the resource bundle.
QShader loadShaderResource(const QString& path);

// One full-screen pass. Passes run in insertion order; the last one usually
// draws into the swapchain. Every pass samples zero or more textures and writes
// one render target, which is the extension point for subtitles and future
// effect passes (the beauty chain is deferred, but the slot stays open).
class RenderPass {
public:
    virtual ~RenderPass() = default;

    virtual const char* name() const = 0;
    // Called every frame: (re)creates targets and pipelines as needed.
    // Returns false when the pass has nothing to draw (e.g. no frame yet).
    virtual bool prepare(QRhiRenderTarget* swapchain) = 0;
    virtual QRhiRenderTarget* renderTarget(QRhiRenderTarget* swapchain) = 0;
    // Uniform / texture uploads for this pass; submitted together with it.
    virtual void update(QRhiResourceUpdateBatch* rub) = 0;
    virtual void draw(QRhiCommandBuffer* cb) = 0;
    virtual void release() = 0;
};

// Owns the shared quad, the sampler and the ordered pass list.
class RenderGraph {
public:
    ~RenderGraph();

    void init(QRhi* rhi);
    void release();
    bool ready() const { return rhi_ != nullptr && quad_ != nullptr; }

    void addPass(std::unique_ptr<RenderPass> p) { passes_.push_back(std::move(p)); }
    void clearPasses();

    void run(QRhiCommandBuffer* cb, QRhiRenderTarget* swapchain);

    // Called once per run(), with the update batch of the first pass that
    // actually executes. Used for the frame upload so that every pass sees the
    // current texture contents.
    void setFrameUpdate(std::function<void(QRhiResourceUpdateBatch*)> fn) {
        frameUpdate_ = std::move(fn);
    }

    // Called after every pass has drawn, with a batch applied outside any pass.
    // GPU readbacks belong here: a readback scheduled before a pass would copy
    // the texture as it was last frame.
    void setPostUpdate(std::function<void(QRhiResourceUpdateBatch*)> fn) {
        postUpdate_ = std::move(fn);
    }

    QRhi* rhi() const { return rhi_; }
    QRhiBuffer* quad() const { return quad_.get(); }
    QRhiSampler* sampler() const { return sampler_.get(); }

private:
    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> quad_;
    std::unique_ptr<QRhiSampler> sampler_;
    bool quadUploaded_ = false;
    std::function<void(QRhiResourceUpdateBatch*)> frameUpdate_;
    std::function<void(QRhiResourceUpdateBatch*)> postUpdate_;
    std::vector<std::unique_ptr<RenderPass>> passes_;
};
