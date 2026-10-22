#define _CRT_SECURE_NO_WARNINGS
#include "AudioSink.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"

#include <atomic>
#include <cstring>

#include "core/Log.h"

struct AudioSink::Impl {
    ma_device device{};
    ma_device_config cfg{};
    ma_pcm_rb rb{};
    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<bool> dirty{false};
    // True while we stopped the device on purpose (pause/shutdown): the
    // "stopped" notification must not trigger a recovery then.
    std::atomic<bool> userStopped{false};
    std::atomic<bool> fed{false};
    bool deviceInit = false;
    bool deviceStarted = false;
    bool rbInit = false;
    int channels = 2;
    int sampleRate = 48000;
};

struct AudioSinkDeviceCallback {
    static void cb(ma_device* dev, void* output, const void* input, ma_uint32 frameCount) {
        (void)input;
        auto* sink = static_cast<AudioSink*>(dev->pUserData);
        auto* impl = sink->impl_;
        if (!impl) return;

        float* dst = static_cast<float*>(output);
        const ma_uint32 ch = static_cast<ma_uint32>(impl->channels);

        ma_uint32 framesRead = frameCount;
        void* readPtr = nullptr;
        if (impl->rbInit &&
            ma_pcm_rb_acquire_read(&impl->rb, &framesRead, &readPtr) == MA_SUCCESS &&
            framesRead > 0) {
            std::memcpy(dst, readPtr, static_cast<size_t>(framesRead) * ch * sizeof(float));
            ma_pcm_rb_commit_read(&impl->rb, framesRead);
        } else {
            framesRead = 0;
        }

        if (framesRead < frameCount) {
            std::memset(dst + static_cast<size_t>(framesRead) * ch, 0,
                        static_cast<size_t>(frameCount - framesRead) * ch * sizeof(float));
            // The first callbacks always find the buffer empty; only count
            // starvation once the producer has started feeding us.
            if (impl->fed.load(std::memory_order_relaxed))
                impl->underruns.fetch_add(1, std::memory_order_relaxed);
        }
        impl->consumed.fetch_add(framesRead, std::memory_order_relaxed);
    }
};

struct AudioSinkNotification {
    static void cb(const ma_device_notification* n) {
        if (!n || !n->pDevice) return;
        auto* sink = static_cast<AudioSink*>(n->pDevice->pUserData);
        if (!sink || !sink->impl_) return;
        switch (n->type) {
        case ma_device_notification_type_stopped:
        case ma_device_notification_type_rerouted:
        case ma_device_notification_type_interruption_ended:
            // A stop we asked for (pause, shutdown) is not a device failure:
            // recovering from it would restart the device behind the player's
            // back and race the teardown (and it makes pause audible again).
            if (sink->impl_->userStopped.load(std::memory_order_relaxed)) break;
            sink->impl_->dirty.store(true, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }
};

AudioSink::AudioSink() = default;

AudioSink::~AudioSink() { shutdown(); }

bool AudioSink::init(int sampleRate, int channels) {
    shutdown();

    sampleRate_ = sampleRate;
    channels_ = channels;
    impl_ = new Impl();
    impl_->channels = channels;
    impl_->sampleRate = sampleRate;

    if (ma_pcm_rb_init(ma_format_f32, static_cast<ma_uint32>(channels),
                       static_cast<ma_uint32>(sampleRate * 2), nullptr, nullptr,
                       &impl_->rb) != MA_SUCCESS) {
        LOGE("audio: ring buffer init failed");
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    impl_->rbInit = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(channels);
    cfg.sampleRate = static_cast<ma_uint32>(sampleRate);
    cfg.dataCallback = &AudioSinkDeviceCallback::cb;
    cfg.notificationCallback = &AudioSinkNotification::cb;
    cfg.pUserData = this;
    cfg.periodSizeInMilliseconds = 10;
    impl_->cfg = cfg;

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        LOGE("audio: device init failed");
        ma_pcm_rb_uninit(&impl_->rb);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    impl_->deviceInit = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        LOGE("audio: device start failed");
        shutdown();
        return false;
    }
    impl_->deviceStarted = true;
    running_ = true;
    if (volume_ != 1.0f) ma_device_set_master_volume(&impl_->device, volume_);
    LOGI("audio: device started (%d Hz, %d ch)", sampleRate_, channels_);
    return true;
}

void AudioSink::setVolume(float volume) {
    volume_ = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    if (impl_ && impl_->deviceInit)
        ma_device_set_master_volume(&impl_->device, volume_);
}

void AudioSink::shutdown() {
    if (!impl_) return;
    running_ = false;
    // Uninit fires the same notification a failed device would; make sure it is
    // not mistaken for a device loss while the player is going away.
    impl_->userStopped.store(true, std::memory_order_relaxed);
    if (impl_->deviceInit) ma_device_uninit(&impl_->device);
    if (impl_->rbInit) ma_pcm_rb_uninit(&impl_->rb);
    delete impl_;
    impl_ = nullptr;
}

void AudioSink::push(const float* interleaved, int frames) {
    if (!impl_ || !impl_->rbInit) return;
    impl_->fed.store(true, std::memory_order_relaxed);

    const size_t ch = static_cast<size_t>(impl_->channels);
    while (frames > 0) {
        ma_uint32 chunk = static_cast<ma_uint32>(frames);
        void* writePtr = nullptr;
        if (ma_pcm_rb_acquire_write(&impl_->rb, &chunk, &writePtr) != MA_SUCCESS) break;
        if (chunk == 0) {
            ma_pcm_rb_commit_write(&impl_->rb, 0);
            break;
        }
        std::memcpy(writePtr, interleaved, static_cast<size_t>(chunk) * ch * sizeof(float));
        ma_pcm_rb_commit_write(&impl_->rb, chunk);
        interleaved += static_cast<size_t>(chunk) * ch;
        frames -= static_cast<int>(chunk);
    }
}

double AudioSink::playedSeconds() const {
    if (!impl_) return 0.0;
    return static_cast<double>(impl_->consumed.load(std::memory_order_relaxed)) / sampleRate_;
}

void AudioSink::reset() {
    if (!impl_) return;
    if (impl_->rbInit) ma_pcm_rb_reset(&impl_->rb);
    impl_->consumed.store(0, std::memory_order_relaxed);
    impl_->fed.store(false, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
}

uint64_t AudioSink::underruns() const {
    if (!impl_) return 0;
    return impl_->underruns.load(std::memory_order_relaxed);
}

void AudioSink::setPaused(bool paused) {
    if (!impl_ || !impl_->deviceInit) return;
    if (paused) {
        if (impl_->deviceStarted) {
            impl_->userStopped.store(true, std::memory_order_relaxed);
            ma_device_stop(&impl_->device);
            impl_->deviceStarted = false;
        }
    } else if (!impl_->deviceStarted) {
        impl_->userStopped.store(false, std::memory_order_relaxed);
        if (ma_device_start(&impl_->device) == MA_SUCCESS) {
            impl_->deviceStarted = true;
        } else {
            // Let the player's recovery path retry instead of staying silent.
            LOGE("audio: cannot restart the device after the pause");
            impl_->dirty.store(true, std::memory_order_relaxed);
        }
    }
}

bool AudioSink::needsRecovery() const {
    return impl_ && impl_->dirty.load(std::memory_order_relaxed);
}

bool AudioSink::recover() {
    if (!impl_) return false;
    impl_->dirty.store(false, std::memory_order_relaxed);
    impl_->userStopped.store(false, std::memory_order_relaxed);
    const bool wasStarted = impl_->deviceStarted;

    // The ring buffer keeps its contents, so playback resumes where it stopped.
    if (impl_->deviceInit) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInit = false;
        impl_->deviceStarted = false;
    }
    const ma_device_config cfg = impl_->cfg;
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        LOGE("audio: device re-init failed");
        running_ = false;
        return false;
    }
    impl_->deviceInit = true;
    ma_device_set_master_volume(&impl_->device, volume_);
    if (wasStarted) {
        if (ma_device_start(&impl_->device) != MA_SUCCESS) {
            LOGE("audio: device restart failed");
            running_ = false;
            return false;
        }
        impl_->deviceStarted = true;
    }
    running_ = true;
    LOGI("audio: output device recovered");
    return true;
}

