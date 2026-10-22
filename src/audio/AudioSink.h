#pragma once
#include <cstdint>

struct AudioSinkDeviceCallback;
struct AudioSinkNotification;

// Thin wrapper around a miniaudio playback device fed from a lock-free ring
// buffer. The device callback also acts as the master clock for A/V sync.
class AudioSink {
public:
    AudioSink();
    ~AudioSink();

    bool init(int sampleRate = 48000, int channels = 2);
    void shutdown();

    // Drops the tail if the ring buffer is full.
    void push(const float* interleaved, int frames);

    // Seconds of audio consumed by the device since the last reset().
    double playedSeconds() const;
    void reset();

    // Stops/resumes the device. Pausing the device freezes playedSeconds(),
    // which is the master clock, and stops the buffered audio immediately.
    void setPaused(bool paused);

    // Master volume, 0..1. Applies immediately and survives recover().
    void setVolume(float volume);
    float volume() const { return volume_; }

    // Callbacks that could not be filled from the ring buffer.
    uint64_t underruns() const;
    // Set by miniaudio when the device was stopped or rerouted (unplugged,
    // default device changed, ...).
    bool needsRecovery() const;
    // Re-opens the device, keeping whatever is still buffered.
    bool recover();

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    bool isRunning() const { return running_; }

    struct Impl;

private:
    friend struct AudioSinkDeviceCallback;
    friend struct AudioSinkNotification;

    Impl* impl_ = nullptr;
    int sampleRate_ = 48000;
    int channels_ = 2;
    bool running_ = false;
    float volume_ = 1.0f;
};
