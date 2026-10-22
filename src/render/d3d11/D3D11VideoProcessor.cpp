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

#include "D3D11VideoProcessor.h"

#include "core/Log.h"

namespace {
void setErr(std::string* err, const char* msg) {
    if (err) *err = msg;
}
}  // namespace

D3D11VideoProcessor::~D3D11VideoProcessor() { release(); }

void D3D11VideoProcessor::release() {
    if (inputView_) { inputView_->Release(); inputView_ = nullptr; }
    if (inputTexture_) { inputTexture_->Release(); inputTexture_ = nullptr; }
    for (int i = 0; i < kOutputCount; ++i) {
        if (outputViews_[i]) { outputViews_[i]->Release(); outputViews_[i] = nullptr; }
        if (outputs_[i]) { outputs_[i]->Release(); outputs_[i] = nullptr; }
        if (outputHandles_[i]) { CloseHandle(outputHandles_[i]); outputHandles_[i] = nullptr; }
    }
    writeIndex_ = 0;
    lastIndex_ = 0;
    inputSlice_ = -1;
    if (processor_) { processor_->Release(); processor_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (videoContext_) { videoContext_->Release(); videoContext_ = nullptr; }
    if (videoDevice_) { videoDevice_->Release(); videoDevice_ = nullptr; }
    if (consumerFence_) { consumerFence_->Release(); consumerFence_ = nullptr; }
    if (fenceHandle_) { CloseHandle(fenceHandle_); fenceHandle_ = nullptr; }
    if (fence_) { fence_->Release(); fence_ = nullptr; }
    if (context4_) { context4_->Release(); context4_ = nullptr; }
    if (device5_) { device5_->Release(); device5_ = nullptr; }
    produced_ = 0;
    width_ = height_ = 0;
    inputFormat_ = DXGI_FORMAT_UNKNOWN;
    inputMatrix_ = -1;
    inputFullRange_ = false;
    device_ = nullptr;
}

bool D3D11VideoProcessor::init(ID3D11Device* device, ID3D11DeviceContext* context,
                               int surfaceWidth, int surfaceHeight,
                               int outputWidth, int outputHeight,
                               DXGI_FORMAT inputFormat, std::string* err) {
    release();
    if (!device || !context) { setErr(err, "no d3d11 device"); return false; }
    device_ = device;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice_)))) {
        setErr(err, "ID3D11VideoDevice unavailable");
        release();
        return false;
    }
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&videoContext_)))) {
        setErr(err, "ID3D11VideoContext unavailable");
        release();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = static_cast<UINT>(surfaceWidth);
    cd.InputHeight = static_cast<UINT>(surfaceHeight);
    cd.OutputWidth = static_cast<UINT>(outputWidth);
    cd.OutputHeight = static_cast<UINT>(outputHeight);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&cd, &enumerator_))) {
        setErr(err, "CreateVideoProcessorEnumerator failed");
        release();
        return false;
    }
    if (FAILED(videoDevice_->CreateVideoProcessor(enumerator_, 0, &processor_))) {
        setErr(err, "CreateVideoProcessor failed");
        release();
        return false;
    }

    // Shared NT-handle textures: the renderer opens these on the scene graph's
    // device. KEYEDMUTEX cannot be combined with NTHANDLE, so synchronisation
    // goes through the fence pair below instead.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(outputWidth);
    td.Height = static_cast<UINT>(outputHeight);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // Drivers disagree about which sharing flags may be combined, so probe the
    // combinations that are known to appear in the wild and keep the first one
    // the driver accepts.
    const UINT candidates[] = {
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
        static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED),
        static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX),
    };
    for (UINT flags : candidates) {
        td.MiscFlags = flags;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &outputs_[0]);
        if (SUCCEEDED(hr)) {
            LOGI("zero-copy: shared texture flags 0x%x accepted", flags);
            break;
        }
        LOGW("zero-copy: shared texture flags 0x%x rejected (hr=0x%08lx)", flags,
             static_cast<unsigned long>(hr));
        outputs_[0] = nullptr;
    }
    if (!outputs_[0]) {
        setErr(err, "no shareable texture configuration accepted");
        release();
        return false;
    }
    if (FAILED(device->CreateTexture2D(&td, nullptr, &outputs_[1]))) {
        setErr(err, "CreateTexture2D (second shared output) failed");
        release();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    od.Texture2D.MipSlice = 0;

    for (int i = 0; i < kOutputCount; ++i) {
        IDXGIResource1* res = nullptr;
        if (FAILED(outputs_[i]->QueryInterface(IID_PPV_ARGS(&res)))) {
            setErr(err, "IDXGIResource1 unavailable");
            release();
            return false;
        }
        const HRESULT hh = res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                   &outputHandles_[i]);
        res->Release();
        if (FAILED(hh) || !outputHandles_[i]) {
            LOGE("zero-copy: CreateSharedHandle hr=0x%08lx", static_cast<unsigned long>(hh));
            setErr(err, "CreateSharedHandle (output texture) failed");
            release();
            return false;
        }
        if (FAILED(videoDevice_->CreateVideoProcessorOutputView(outputs_[i], enumerator_, &od,
                                                                &outputViews_[i]))) {
            setErr(err, "CreateVideoProcessorOutputView failed");
            release();
            return false;
        }
    }

    // Fences need D3D11.4 (Windows 10 1703+). Without them there is no way to
    // synchronise two devices, so treat the whole path as unavailable.
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5_))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&context4_)))) {
        setErr(err, "ID3D11Device5/ID3D11DeviceContext4 unavailable");
        release();
        return false;
    }
    if (FAILED(device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_)))) {
        setErr(err, "CreateFence failed");
        release();
        return false;
    }
    if (FAILED(fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle_)) ||
        !fenceHandle_) {
        setErr(err, "CreateSharedHandle (fence) failed");
        release();
        return false;
    }

    // Progressive input, one frame per output frame.
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_, 0,
                                                      D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamOutputRate(processor_, 0,
                                                     D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL,
                                                     FALSE, nullptr);

    // Cut the aligned padding off the decoded surface instead of letting the
    // processor read it (3840x2176 surface -> 3840x2160 image).
    RECT visible{ 0, 0, outputWidth, outputHeight };
    videoContext_->VideoProcessorSetStreamSourceRect(processor_, 0, TRUE, &visible);
    videoContext_->VideoProcessorSetStreamDestRect(processor_, 0, TRUE, &visible);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_, TRUE, &visible);

    // BT.709 limited range in, full-range RGB out (the shader path overrides
    // this per file via setInputColorSpace()).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.Usage = 0;
    inCs.RGB_Range = 0;
    inCs.YCbCr_Matrix = 1;  // BT.709
    inCs.YCbCr_xvYCC = 0;
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_, 0, &inCs);
    inputMatrix_ = 1;
    inputFullRange_ = false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.RGB_Range = 0;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetOutputColorSpace(processor_, &outCs);

    width_ = outputWidth;
    height_ = outputHeight;
    inputFormat_ = inputFormat;
    return true;
}

void D3D11VideoProcessor::setInputColorSpace(int matrix, bool fullRange) {
    if (!videoContext_ || !processor_) return;
    if (matrix == inputMatrix_ && fullRange == inputFullRange_) return;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
    cs.Usage = 0;
    cs.RGB_Range = 0;
    cs.YCbCr_Matrix = (matrix == 0) ? 0 : 1;   // 0 = BT.601, 1 = BT.709
    cs.YCbCr_xvYCC = 0;
    cs.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_, 0, &cs);
    inputMatrix_ = matrix == 0 ? 0 : 1;
    inputFullRange_ = fullRange;
    LOGI("zero-copy: input %s%s", inputMatrix_ == 0 ? "bt601" : "bt709",
         fullRange ? " full-range" : " limited-range");
}

bool D3D11VideoProcessor::setConsumerFence(HANDLE handle) {    if (consumerFence_) { consumerFence_->Release(); consumerFence_ = nullptr; }
    if (!handle || !device5_) return false;
    if (FAILED(device5_->OpenSharedFence(handle, IID_PPV_ARGS(&consumerFence_)))) {
        LOGE("zero-copy: OpenSharedFence(consumer) failed");
        consumerFence_ = nullptr;
        return false;
    }
    return true;
}

bool D3D11VideoProcessor::convert(ID3D11Texture2D* src, int arraySlice,
                                  uint64_t consumerDone, std::string* err) {
    if (!processor_ || !src) { setErr(err, "video processor not ready"); return false; }

    // Serialise with the decoder thread: the immediate context is shared.
    struct Guard {
        void (*lock)(void*);
        void (*unlock)(void*);
        void* ctx;
        ~Guard() { if (unlock) unlock(ctx); }
    } guard{ lock_, unlock_, lockCtx_ };
    if (lock_) lock_(lockCtx_);

    if (device_) {
        const HRESULT pre = device_->GetDeviceRemovedReason();
        if (pre != S_OK) {
            LOGW("zero-copy: device already removed before blt (0x%08lx)",
                 static_cast<unsigned long>(pre));
        }
    }

    if (src != inputTexture_ || arraySlice != inputSlice_) {
        if (inputView_) { inputView_->Release(); inputView_ = nullptr; }
        if (inputTexture_) { inputTexture_->Release(); inputTexture_ = nullptr; }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = static_cast<UINT>(arraySlice);

        if (FAILED(videoDevice_->CreateVideoProcessorInputView(src, enumerator_, &ivd,
                                                               &inputView_))) {
            setErr(err, "CreateVideoProcessorInputView failed");
            return false;
        }
        src->AddRef();
        inputTexture_ = src;
        inputSlice_ = arraySlice;
    }

    // GPU-side wait: do not overwrite a buffer the renderer's device may still
    // be sampling. No CPU stall, the wait is queued on our command stream.
    if (context4_ && consumerFence_) context4_->Wait(consumerFence_, consumerDone);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = inputView_;

    HRESULT hr = videoContext_->VideoProcessorBlt(processor_, outputViews_[writeIndex_], 0, 1,
                                                  &stream);
    if (SUCCEEDED(hr)) {
        lastIndex_ = writeIndex_;
        writeIndex_ = (writeIndex_ + 1) % kOutputCount;
        // Tell the renderer's device that this output texture is ready.
        if (context4_ && fence_) context4_->Signal(fence_, ++produced_);
    }
    if (device_) {
        const HRESULT post = device_->GetDeviceRemovedReason();
        if (post != S_OK) {
            LOGW("zero-copy: device removed around blt (0x%08lx, blt hr=0x%08lx)",
                 static_cast<unsigned long>(post), static_cast<unsigned long>(hr));
        }
    }
    if (FAILED(hr)) {
        HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        LOGW("zero-copy: VideoProcessorBlt failed hr=0x%08lx reason=0x%08lx",
             static_cast<unsigned long>(hr), static_cast<unsigned long>(reason));
        setErr(err, "VideoProcessorBlt failed");
        return false;
    }
    return true;
}
