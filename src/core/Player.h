#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AudioDecoder.h"
#include "FrameQueue.h"
#include "VideoDecoder.h"
#include "VideoFrame.h"

struct AVFormatContext;
class AudioSink;
class Recorder;

// Owns the demux/decode thread and the master clock.
// Render side pulls frames with frameForNow(); everything else is thread safe.
class Player {
public:
    // Stops after this many decoded frames are pending presentation. Kept small
    // so a seek only has to throw away a couple of frames, and so the decoder
    // surface pool is not exhausted by a long queue.
    static constexpr size_t kDisplayQueueSize = 3;
    static constexpr size_t kDecodeQueueSize = 12;

    // Frames later than this behind the clock are skipped instead of shown.
    static constexpr double kLateFrameSeconds = 0.2;

    struct AudioTrack {
        int streamIndex = -1;
        std::string language;
        std::string title;
        int channels = 0;
        int sampleRate = 0;
    };

    Player();
    ~Player();

    bool open(const std::string& path, std::string* err);
    void close();

    void play();
    void pause();
    void togglePause();
    bool isPaused() const { return paused_.load(); }

    void seek(double seconds);

    // 0.25x .. 4x with pitch preserved (atempo). The worker re-anchors the
    // clock at the current position, so this is safe to call while playing.
    void setSpeed(double speed);
    // The requested rate; it becomes effective a moment later on the worker.
    double speed() const {
        return speedPending_.load() ? pendingSpeed_.load() : speed_.load();
    }
    // Precise seek decodes from the preceding keyframe up to the target and
    // drops everything before it. Disable to start on the keyframe instead.
    void setSeekPrecise(bool on) { seekPrecise_.store(on); }
    bool seekPrecise() const { return seekPrecise_.load(); }

    void setLoop(bool on) { loop_.store(on); }
    bool isLooping() const { return loop_.load(); }

    // 0..1, applied to the output device (survives device recovery/re-open).
    void setVolume(float volume);
    float volume() const { return volume_.load(); }
    // True once the last frame has been presented (and loop is off).
    bool isFinished() const { return finished_.load(); }

    double position() const;
    double duration() const { return duration_.load(); }
    bool hasVideo() const { return videoIndex_ >= 0; }
    bool hasAudio() const { return audioIndex_.load() >= 0; }

    // --- offline export (P5.2) ---
    // Decoding without the playback clock: the output device is silenced and the
    // caller pulls frames in order with pullFrame(), rendering each one itself.
    // The media timestamps stay untouched, so a recording made this way is the
    // same as a real-time one, only faster.
    void setOffline(bool on);
    bool offline() const { return offline_.load(); }
    // Next frame in decode order (never reordered or dropped). Returns false at
    // end of file, or after a short stall (the decoder gave us nothing).
    bool pullFrame(VideoFramePtr& out);
    bool seeking() const { return seeking_.load(); }
    int width() const;
    int height() const;
    // Clockwise quarter turns the stored image needs before display (0..3),
    // taken from the stream's display matrix (phone videos). |mirrored| is the
    // horizontal flip some capture devices write into the same matrix.
    int rotation() const;
    bool mirrored() const;

    // --- audio tracks ---
    std::vector<AudioTrack> audioTracks() const;
    int currentAudioTrack() const { return audioIndex_.load(); }
    // Takes effect on the worker thread; the stream is re-seeked so the new
    // track stays in sync with the video.
    bool selectAudioTrack(int streamIndex);

    // --- recording (P5) ---
    // Captures the playback to |path| as MP4 (H.264 + AAC). The renderer pushes
    // the chain output and drainAudio() pushes the PCM, both tagged with the
    // media clock, so the file follows the frames actually shown. |path| is
    // opened immediately: a failure (unwritable directory, no encoder) is
    // reported through |err| and nothing is recorded.
    bool startRecording(const std::string& path, std::string* err);
    void stopRecording();
    bool isRecording() const;
    // Clip export: stop writing after |seconds| of the file (see Recorder).
    void setRecordingLimit(double seconds);
    bool recordingLimitReached() const;
    // The render thread takes a reference; it stays valid while the encoder
    // thread drains, even if stopRecording() runs meanwhile.
    std::shared_ptr<Recorder> recorder() const;

    // --- render thread API ---
    VideoFramePtr frameForNow();
    VideoFramePtr currentFrame() const;
    bool hardwareDecoding() const;
    void setHardwarePassthrough(bool on);
    ID3D11Device* d3dDevice() const;
    ID3D11DeviceContext* d3dContext() const;
    bool d3dLock(void (**lock)(void*), void (**unlock)(void*), void** lockCtx) const;

    // D3D11 device the decoder created; the renderer opens its shared output
    // textures on the scene graph's device.
    void* d3d11DeviceRaw() const;

    void setFrameCallback(std::function<void()> cb) { frameCb_ = std::move(cb); }
    uint64_t droppedFrames() const { return dropped_.load(); }
    uint64_t audioUnderruns() const;

private:
    void worker();
    void drainVideo();
    void drainAudio();
    void doSeek(double target);
    void primeTo(double target);
    void doAudioSwitch(int streamIndex);
    void closeLocked();
    double masterClock() const;
    bool audioClockActive() const;

    AVFormatContext* fmt_ = nullptr;
    std::atomic<int> videoIndex_{-1};
    std::atomic<int> audioIndex_{-1};
    std::unique_ptr<VideoDecoder> videoDec_;
    std::unique_ptr<AudioDecoder> audioDec_;
    std::unique_ptr<AudioSink> sink_;
    std::vector<AudioTrack> audioTracks_;

    // Decoded frames waiting to be presented, plus the handful the renderer is
    // about to show. Splitting them keeps a seek from waiting for the decoder.
    FrameQueue<VideoFramePtr> videoQueue_{kDecodeQueueSize};
    FrameQueue<VideoFramePtr> displayQueue_{kDisplayQueueSize};
    std::thread worker_;

    // Guards the public API against open()/close() running on the GUI thread
    // while the render thread pulls frames. Never taken by the worker thread.
    mutable std::mutex apiMx_;
    mutable std::mutex mx_;
    std::condition_variable cv_;

    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
    // Offline export: no output device traffic, frames pulled by the exporter.
    std::atomic<bool> offline_{false};
    std::atomic<bool> seekPending_{false};
    std::atomic<bool> seekPrecise_{true};
    std::atomic<bool> seeking_{false};
    std::atomic<bool> demuxDone_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> loop_{false};
    std::atomic<float> volume_{1.0f};
    // True once audio has actually been pushed after the last open/seek, i.e.
    // the sink is a valid clock. Stays false for video-only files or when the
    // audio track ends before the seek target.
    std::atomic<bool> audioSynced_{false};
    std::atomic<double> seekTarget_{0.0};
    std::atomic<double> clockBase_{0.0};
    std::atomic<double> pausedAccum_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<uint64_t> dropped_{0};

    // Clock extrapolation state used when the audio track ends before the video
    // (or the device stops delivering): remembers the last position the audio
    // clock reached and when it got there.
    mutable std::atomic<double> audioHigh_{0.0};
    mutable std::atomic<int64_t> audioHighWallNs_{0};
    std::atomic<double> finishClock_{0.0};

    std::atomic<bool> audioSwitchPending_{false};
    std::atomic<int> pendingAudioIndex_{-1};

    std::atomic<bool> speedPending_{false};
    std::atomic<double> pendingSpeed_{1.0};
    std::atomic<double> speed_{1.0};

    std::chrono::steady_clock::time_point playStart_{};
    std::chrono::steady_clock::time_point pauseBegin_{};

    mutable std::mutex curMx_;
    VideoFramePtr current_;
    std::function<void()> frameCb_;

    // Recording: the audio path reads it on every decoded chunk, so the pointer
    // is shared and swapped under its own lock (never taken by the worker).
    mutable std::mutex recMx_;
    std::shared_ptr<Recorder> rec_;
};
