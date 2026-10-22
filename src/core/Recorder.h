#pragma once
#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVStream;
struct SwsContext;

// Writes what the player shows into an MP4: the renderer hands over the chain
// output (RGBA, display size, read back from the GPU) and the decode thread
// hands over the PCM it is about to play. Both go through a queue that the
// encoder thread drains, so recording never blocks presentation.
//
// Video is H.264 through the first hardware encoder that opens (NVENC, Quick
// Sync, AMF) and libopenh264 otherwise; audio is AAC. Timestamps come from the
// media clock, not from the wall clock: a 24 fps file recorded on a 60 Hz
// display is written as 24 fps, and a dropped display frame leaves a gap
// instead of shifting the rest of the file.
class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens the muxer and both encoders. |width| x |height| is the display size
    // (display matrix already applied). |fps| is only a hint for the encoder
    // and the bitrate: frame timestamps drive the file. |hasAudio| adds an AAC
    // track fed by pushAudio().
    bool start(const QString& path, int width, int height, double fps, bool hasAudio,
               int sampleRate, int channels, std::string* err);
    // Stops the worker, flushes both encoders and writes the trailer. Safe to
    // call twice and safe to call while the render thread still pushes frames.
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

    // Render thread. |rgba| is tightly packed RGBA8 in sRGB, |pts| is the media
    // timestamp of the frame it came from.
    void pushVideo(QByteArray rgba, int width, int height, double pts);
    // Decode thread. |pcm| is interleaved float, |pts| the first sample's time.
    void pushAudio(const float* pcm, int frames, double pts);

    QString path() const { return path_; }
    QString codec() const { return codec_; }
    uint64_t videoFrames() const { return videoFrames_.load(std::memory_order_relaxed); }
    uint64_t audioFrames() const { return audioFrames_.load(std::memory_order_relaxed); }
    uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }
    int64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }

    // Stops writing after |seconds| of the file's timeline (clip export). Frames
    // past the limit are skipped without being counted as drops, so the caller
    // can keep recording for a moment while the last in-flight frames are
    // presented and still get an exact cut.
    void setLimit(double seconds);
    bool limitReached() const { return limitHit_.load(std::memory_order_relaxed); }
    // Offline export: pushVideo() waits for encoder room instead of dropping.
    void setBackpressure(bool on) { backpressure_.store(on, std::memory_order_relaxed); }

private:
    struct VideoJob {
        QByteArray rgba;
        int width = 0;
        int height = 0;
        double pts = 0.0;
    };

    void worker();
    bool openVideo(int w, int h, double fps, std::string* err);
    bool openAudio(int rate, int channels, std::string* err);
    void encodeVideo(VideoJob& job);
    // Maps a media timestamp onto the file's timeline. A seek or a loop makes the
    // media clock jump; both streams share |shift_| so the jump becomes a cut
    // instead of a muxer error (or a permanent A/V offset).
    double mapTime(double pts, double expected, double lastOut);
    // Accumulates interleaved float PCM and encodes whole AAC frames.
    void queueAudio(std::vector<float> pcm, double pts);
    void encodeAudioBlock(const float* pcm, int frames, double pts);
    void drainEncoder(AVCodecContext* enc, AVStream* st);
    void flushEncoders();
    void closeAll();
    void logStats();

    AVFormatContext* fmt_ = nullptr;
    AVStream* vst_ = nullptr;
    AVStream* ast_ = nullptr;
    AVCodecContext* venc_ = nullptr;
    AVCodecContext* aenc_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* vframe_ = nullptr;
    AVFrame* aframe_ = nullptr;
    QString path_;
    QString codec_;
    int width_ = 0;
    int height_ = 0;
    int sampleRate_ = 48000;
    int channels_ = 2;
    // First timestamp seen (video or audio): every stream is written relative
    // to it so the file starts at 0 and A/V keeps its relative offset.
    double t0_ = -1.0;
    // Timeline rebase (see mapTime) plus the last written position of each
    // stream, used to detect a discontinuity.
    double shift_ = 0.0;
    double lastVideoOut_ = -1.0;
    double lastAudioOut_ = -1.0;
    double frameDur_ = 1.0 / 30.0;
    uint64_t cuts_ = 0;
    // Samples waiting for a full AAC frame, interleaved, plus the time of the
    // first one.
    std::vector<float> audioPending_;
    double audioPendingPts_ = 0.0;

    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::deque<VideoJob> videoQ_;
    std::deque<std::pair<std::vector<float>, double>> audioQ_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};

    std::atomic<uint64_t> videoFrames_{0};
    std::atomic<uint64_t> audioFrames_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<int64_t> bytes_{0};
    std::atomic<double> limit_{-1.0};
    std::atomic<bool> limitHit_{false};
    std::atomic<bool> backpressure_{false};
    // Signalled when the encoder takes a frame, so an offline export that waits
    // for room can wake up.
    std::condition_variable cvSpace_;
    int64_t lastLogMs_ = 0;
};
