#pragma once

#include <d3d11_1.h>
#include <d3d11_4.h>

#include <cstdint>
#include <string>

// Converts decoded NV12/P010 textures into an RGBA texture on the GPU using the
// D3D11 fixed-function video processor, so decoded frames never leave VRAM.
//
// The output textures are created as shared NT-handle resources on the decoder's
// device. The renderer opens them on the RHI device, which is the only way to
// hand a texture between two ID3D11Devices without undefined behaviour, and an
// ID3D11Fence pair provides GPU-side synchronisation in both directions.
class D3D11VideoProcessor {
public:
    // The output is double buffered: frame N is drawn while frame N+1 is being
    // converted into the other texture, so a texture is never read by the
    // compositor and written by the video processor at the same time.
    static constexpr int kOutputCount = 2;

    ~D3D11VideoProcessor();

    // device/context must be the ones owned by the FFmpeg decoder, otherwise
    // the decoded textures cannot be used as input views.
    //
    // surfaceWidth/Height describe the decoded texture (height-aligned, e.g.
    // 3840x2176); outputWidth/Height describe the visible video size and are
    // also the size of output(). D3D11 video processors read the whole surface
    // unless a source rect is set, so the two are kept explicit.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context,
              int surfaceWidth, int surfaceHeight,
              int outputWidth, int outputHeight,
              DXGI_FORMAT inputFormat, std::string* err = nullptr);
    void release();

    // Describe the input samples (matrix 0 = BT.601, 1 = BT.709; range). The
    // video processor only knows these two matrices, which is why BT.2020/HDR
    // sources take the shader path instead. Cheap to call per frame: repeated
    // values are a no-op.
    void setInputColorSpace(int matrix, bool fullRange);

    // Convert one array slice of |src| into the next output texture.
    //
    // |consumerDone| is a value of the consumer fence: the blt waits (on the
    // GPU) until the renderer is finished reading the buffer it reuses. After a
    // successful blt the producer fence is signalled with producedValue().
    bool convert(ID3D11Texture2D* src, int arraySlice, uint64_t consumerDone,
                 std::string* err = nullptr);

    // Recursive lock protecting the shared immediate context (see
    // AVD3D11VADeviceContext::lock). Required when another thread decodes
    // concurrently with convert().
    void setLock(void (*lock)(void*), void (*unlock)(void*), void* lockCtx) {
        lock_ = lock;
        unlock_ = unlock;
        lockCtx_ = lockCtx;
    }

    // Shared NT handle of outputTexture(index), for OpenSharedResource1().
    HANDLE outputHandle(int index) const {
        return index >= 0 && index < kOutputCount ? outputHandles_[index] : nullptr;
    }
    // Shared NT handle of the fence signalled after every blt.
    HANDLE producerFenceHandle() const { return fenceHandle_; }
    // Open the renderer's fence on our device so convert() can wait on it.
    bool setConsumerFence(HANDLE handle);
    uint64_t producedValue() const { return produced_; }

    // Texture written by the last successful convert().
    ID3D11Texture2D* output() const { return outputs_[lastIndex_]; }
    ID3D11Texture2D* outputTexture(int index) const {
        return outputs_[index >= 0 && index < kOutputCount ? index : 0];
    }
    int outputIndex() const { return lastIndex_; }
    bool valid() const { return processor_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* enumerator_ = nullptr;
    ID3D11VideoProcessor* processor_ = nullptr;

    ID3D11Texture2D* outputs_[kOutputCount] = { nullptr, nullptr };
    ID3D11VideoProcessorOutputView* outputViews_[kOutputCount] = { nullptr, nullptr };
    HANDLE outputHandles_[kOutputCount] = { nullptr, nullptr };
    int writeIndex_ = 0;
    int lastIndex_ = 0;

    ID3D11VideoProcessorInputView* inputView_ = nullptr;
    ID3D11Texture2D* inputTexture_ = nullptr;
    int inputSlice_ = -1;

    // Cross-device synchronisation (D3D11.4 fences, both shared NT handles).
    ID3D11Device5* device5_ = nullptr;
    ID3D11DeviceContext4* context4_ = nullptr;
    ID3D11Fence* fence_ = nullptr;          // signalled after every blt
    HANDLE fenceHandle_ = nullptr;
    ID3D11Fence* consumerFence_ = nullptr;  // signalled by the renderer
    uint64_t produced_ = 0;

    int width_ = 0;  // output / visible size
    int height_ = 0;
    DXGI_FORMAT inputFormat_ = DXGI_FORMAT_UNKNOWN;
    int inputMatrix_ = -1;      // last matrix applied to the stream (0/1)
    bool inputFullRange_ = false;
    ID3D11Device* device_ = nullptr;  // non-owning, for diagnostics

    void (*lock_)(void*) = nullptr;
    void (*unlock_)(void*) = nullptr;
    void* lockCtx_ = nullptr;
};
