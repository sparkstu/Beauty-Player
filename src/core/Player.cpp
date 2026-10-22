#include "Player.h"

#include "Log.h"
#include "Recorder.h"
#include "audio/AudioSink.h"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include <algorithm>

namespace {
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

std::string metaStr(AVDictionary* dict, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? std::string(e->value) : std::string();
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

Player::Player() = default;

Player::~Player() { close(); }

bool Player::open(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lk(apiMx_);
    closeLocked();

    // A rate picked before the file was loaded applies from the first frame.
    if (speedPending_.exchange(false)) speed_ = pendingSpeed_.load();

    int ret = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (err) *err = "cannot open file: " + avErr(ret);
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        if (err) *err = "cannot read stream info";
        closeLocked();
        return false;
    }

    // av_find_best_stream() returns a negative AVERROR, normalise to -1 so that
    // logs and hasVideo()/hasAudio() stay meaningful.
    const int vi = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    videoIndex_ = vi < 0 ? -1 : vi;
    audioIndex_ = ai < 0 ? -1 : ai;

    if (videoIndex_.load() < 0 && audioIndex_.load() < 0) {
        if (err) *err = "file has no playable stream";
        closeLocked();
        return false;
    }

    // Enumerate every audio stream so the UI can offer a track picker.
    audioTracks_.clear();
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        const AVStream* st = fmt_->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AudioTrack t;
        t.streamIndex = static_cast<int>(i);
        t.language = metaStr(st->metadata, "language");
        t.title = metaStr(st->metadata, "title");
        t.channels = st->codecpar->ch_layout.nb_channels;
        t.sampleRate = st->codecpar->sample_rate;
        audioTracks_.push_back(t);
    }

    if (videoIndex_.load() >= 0) {
        videoDec_ = std::make_unique<VideoDecoder>();
        if (!videoDec_->open(fmt_->streams[videoIndex_.load()], err)) {
            closeLocked();
            return false;
        }
    }

    if (audioIndex_.load() >= 0) {
        audioDec_ = std::make_unique<AudioDecoder>();
        std::string aerr;
        if (!audioDec_->open(fmt_->streams[audioIndex_.load()], &aerr)) {
            LOGW("audio: %s (continuing without audio)", aerr.c_str());
            audioDec_.reset();
            audioIndex_ = -1;
        } else {
            if (speed_.load() != 1.0) audioDec_->setSpeed(speed_.load());
            sink_ = std::make_unique<AudioSink>();
            if (!sink_->init(audioDec_->outputSampleRate(), audioDec_->outputChannels())) {
                sink_.reset();
            } else if (volume_.load() != 1.0f) {
                sink_->setVolume(volume_.load());
            }
        }
    }

    duration_ = fmt_->duration != AV_NOPTS_VALUE
                    ? static_cast<double>(fmt_->duration) / AV_TIME_BASE
                    : 0.0;

    clockBase_ = 0.0;
    pausedAccum_ = 0.0;
    playStart_ = std::chrono::steady_clock::now();
    pauseBegin_ = playStart_;
    audioHigh_ = 0.0;
    audioHighWallNs_ = nowNs();
    paused_ = false;
    demuxDone_ = false;
    finished_ = false;
    seekTarget_ = 0.0;
    abort_ = false;
    // Hold the clock until the first frame is on screen.
    seeking_ = true;
    audioSynced_ = false;
    videoQueue_.reset();
    videoQueue_.flush();
    displayQueue_.reset();
    displayQueue_.flush();

    worker_ = std::thread(&Player::worker, this);
    LOGI("opened: video=%d audio=%d tracks=%zu duration=%.2fs", videoIndex_.load(),
         audioIndex_.load(), audioTracks_.size(), duration_.load());
    return true;
}

void Player::close() {
    std::lock_guard<std::mutex> lk(apiMx_);
    closeLocked();
}

void Player::closeLocked() {
    // A recording cannot outlive the file it captures.
    {
        std::shared_ptr<Recorder> rec;
        {
            std::lock_guard<std::mutex> rl(recMx_);
            rec.swap(rec_);
        }
        if (rec) rec->stop();
    }

    abort_ = true;
    videoQueue_.abort();
    displayQueue_.abort();
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    if (sink_) {
        sink_->shutdown();
        sink_.reset();
    }
    videoDec_.reset();
    audioDec_.reset();
    audioTracks_.clear();

    {
        std::lock_guard lk(curMx_);
        current_.reset();
    }
    videoQueue_.flush();
    displayQueue_.flush();

    if (fmt_) avformat_close_input(&fmt_);
    videoIndex_ = -1;
    audioIndex_ = -1;
    duration_ = 0.0;
    demuxDone_ = false;
    finished_ = false;
    seeking_ = false;
    audioSynced_ = false;
    audioSwitchPending_ = false;
    pendingAudioIndex_ = -1;
    abort_ = false;
    videoQueue_.reset();
    displayQueue_.reset();
}

void Player::worker() {
    AVPacket* pkt = av_packet_alloc();

    while (!abort_.load()) {
        if (audioSwitchPending_.exchange(false)) doAudioSwitch(pendingAudioIndex_.load());
        if (speedPending_.exchange(false)) {
            // Re-anchor the clock at the current position under the new rate;
            // the audio graph and the sink are rebuilt by the seek.
            const double at = masterClock();
            speed_ = pendingSpeed_.load();
            if (audioDec_) audioDec_->setSpeed(speed_.load());
            doSeek(at);
        }
        if (seekPending_.exchange(false)) doSeek(seekTarget_.load());

        {
            std::unique_lock lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(8), [&] {
                return abort_.load() || seekPending_.load() || audioSwitchPending_.load() ||
                       speedPending_.load() || !paused_.load();
            });
        }
        if (abort_.load()) break;

        // miniaudio tells us when the output device was unplugged or rerouted;
        // rebuilding it here (not on the audio thread) keeps playback going.
        // The ring buffer and the played-sample counter survive, so the clock
        // does not jump.
        if (sink_ && sink_->needsRecovery()) sink_->recover();

        // Note: decoding continues while paused (back-pressured by the queues)
        // so a scrub or a resume already has frames to show.

        // Back-pressure on the *decoded* queue; the display queue is refilled by
        // the render thread and only holds what is about to be shown.
        if (videoQueue_.size() >= videoQueue_.capacity()) {
            std::unique_lock lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(4), [&] {
                return abort_.load() || seekPending_.load() || audioSwitchPending_.load() ||
                       speedPending_.load();
            });
            continue;
        }

        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF) LOGW("demux: %s", avErr(ret).c_str());
            if (videoDec_) {
                videoDec_->sendPacket(nullptr);
                drainVideo();
            }
            if (audioDec_) {
                audioDec_->sendPacket(nullptr);
                drainAudio();
            }
            demuxDone_ = true;
            // Stay alive: loop playback or a seek needs to read from the file
            // again, and av_read_frame() recovers after av_seek_frame().
            std::unique_lock lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return abort_.load() || seekPending_.load() || audioSwitchPending_.load();
            });
            continue;
        }

        if (pkt->stream_index == videoIndex_.load() && videoDec_) {
            videoDec_->sendPacket(pkt);
            drainVideo();
        } else if (pkt->stream_index == audioIndex_.load() && audioDec_) {
            audioDec_->sendPacket(pkt);
            drainAudio();
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

void Player::drainVideo() {
    for (;;) {
        VideoFramePtr f = videoDec_->receiveFrame();
        if (!f) break;
        if (!videoQueue_.push(f)) return;
        if (frameCb_) frameCb_();
    }
}

void Player::drainAudio() {
    std::vector<float> pcm;
    for (;;) {
        double pts = 0.0;
        const int frames = audioDec_->receive(pcm, &pts);
        if (frames <= 0) break;

        // Drop everything before the seek target so the first sample we play is
        // the one the picture is already showing.
        if (seeking_.load() && seekPrecise_.load() && pts + 0.010 < seekTarget_.load()) continue;

        if (!audioSynced_.load()) {
            // First audio sample after open/seek: it now owns the master clock.
            clockBase_ = pts;
            audioHigh_ = 0.0;
            audioHighWallNs_ = nowNs();
            audioSynced_ = true;
            seeking_ = false;
            LOGI("clock: anchored on audio at %.3fs", pts);
        }
        if (!sink_) {
            // nothing to play through
        } else if (!offline_.load()) {
            sink_->push(pcm.data(), frames);
        }
        // The recorder gets the same samples the device is about to play, so
        // the file's audio follows the master clock like the picture does.
        if (auto rec = recorder()) rec->pushAudio(pcm.data(), frames, pts);
    }
}

void Player::setOffline(bool on) {
    if (offline_.exchange(on) == on) return;
    // Silence the device while exporting (the samples still reach the recorder),
    // but leave a paused player paused: pause() owns the device state then.
    if (sink_ && !paused_.load()) sink_->setPaused(on);
    cv_.notify_all();
}

bool Player::pullFrame(VideoFramePtr& out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(apiMx_);
            VideoFramePtr f;
            if (videoQueue_.tryPop(f)) {
                // During an export frameForNow() is never called, so the seek
                // bookkeeping has to happen here.
                if (seeking_.load()) {
                    if (f->pts + 0.002 < seekTarget_.load()) continue;  // pre-roll
                    seeking_ = false;
                }
                out = std::move(f);
                return true;
            }
        }
        if (demuxDone_.load() && videoQueue_.size() == 0) return false;
        if (std::chrono::steady_clock::now() > deadline) {
            // The decoder has not produced anything for 5 s: treated as the end
            // of what we can export (a broken file should not hang the UI).
            LOGW("offline: no frame for 5 s, stopping at %.3fs", position());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Player::doSeek(double target) {    if (!fmt_) return;
    const int stream = videoIndex_.load() >= 0 ? videoIndex_.load() : audioIndex_.load();
    if (stream < 0) return;

    if (target < 0.0) target = 0.0;
    const double d = duration_.load();
    if (d > 0.0) target = std::min(target, d);

    const AVRational tb = fmt_->streams[stream]->time_base;
    const int64_t ts = static_cast<int64_t>(target / av_q2d(tb));

    const int ret = av_seek_frame(fmt_, stream, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) LOGW("seek failed: %s", avErr(ret).c_str());

    if (videoDec_) videoDec_->flush();
    if (audioDec_) audioDec_->flush();

    videoQueue_.flush();
    displayQueue_.flush();
    {
        std::lock_guard lk(curMx_);
        current_.reset();
    }
    if (sink_) sink_->reset();

    clockBase_ = target;
    seekTarget_ = target;
    playStart_ = std::chrono::steady_clock::now();
    pauseBegin_ = playStart_;
    pausedAccum_ = 0.0;
    audioHigh_ = 0.0;
    audioHighWallNs_ = nowNs();
    demuxDone_ = false;
    finished_ = false;
    seeking_ = true;
    audioSynced_ = false;

    // Decode past everything before the target here, on the demux thread. The
    // renderer then only ever sees a frame at (or after) the seek position, and
    // a long GOP costs one decode burst instead of stalling the scene graph.
    if (seekPrecise_.load()) primeTo(target);

    LOGI("seek: %.3fs (%s)", target, seekPrecise_.load() ? "precise" : "keyframe");
}

void Player::primeTo(double target) {
    if (!videoDec_ || !fmt_) return;

    AVPacket* pkt = av_packet_alloc();
    bool drained = false;
    int decoded = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);

    for (;;) {
        if (abort_.load()) break;

        if (VideoFramePtr f = videoDec_->receiveFrame()) {
            ++decoded;
            if (f->pts + 0.002 < target) continue;
            videoQueue_.push(f);
            break;
        }

        if (drained) break;
        if (!pkt || av_read_frame(fmt_, pkt) < 0) {
            videoDec_->sendPacket(nullptr);
            drained = true;
            continue;
        }
        if (pkt->stream_index == videoIndex_.load()) videoDec_->sendPacket(pkt);
        av_packet_unref(pkt);

        if (std::chrono::steady_clock::now() > deadline) {
            LOGW("seek: still priming after 2s, showing a late frame");
            break;
        }
    }

    av_packet_free(&pkt);
    LOGI("seek: %.3fs primed (%d frames decoded)", target, decoded);
}

void Player::doAudioSwitch(int streamIndex) {
    if (!fmt_ || streamIndex < 0 || streamIndex >= static_cast<int>(fmt_->nb_streams)) return;
    if (streamIndex == audioIndex_.load()) return;

    const AVStream* st = fmt_->streams[streamIndex];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return;

    auto dec = std::make_unique<AudioDecoder>();
    std::string err;
    if (!dec->open(st, &err)) {
        LOGE("audio: cannot switch to stream %d: %s", streamIndex, err.c_str());
        return;
    }

    audioDec_ = std::move(dec);
    audioIndex_ = streamIndex;
    if (!sink_) {
        sink_ = std::make_unique<AudioSink>();
        if (!sink_->init(audioDec_->outputSampleRate(), audioDec_->outputChannels())) {
            sink_.reset();
            LOGE("audio: no output device, track switched silently");
        }
    } else if (sink_) {
        sink_->reset();
    }

    // Re-seek so the new track starts at the picture we are already showing.
    const double at = masterClock();
    LOGI("audio: switched to stream %d (%s), re-syncing at %.3fs", streamIndex,
         st->metadata ? metaStr(st->metadata, "language").c_str() : "", at);
    doSeek(at);
}

void Player::play() {
    const bool wasFinished = finished_.load();
    if (wasFinished) {
        seek(0.0);
        finished_ = false;
    }
    if (sink_) sink_->setPaused(false);
    if (!paused_.exchange(false)) {
        cv_.notify_all();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    pausedAccum_ = pausedAccum_.load() + std::chrono::duration<double>(now - pauseBegin_).count();
    audioHighWallNs_ = nowNs();
    cv_.notify_all();
}

void Player::pause() {
    if (paused_.exchange(true)) return;
    // Stop the output device: otherwise the buffered audio keeps playing (and
    // keeps the master clock running) for a couple of seconds after the click.
    if (sink_) sink_->setPaused(true);
    pauseBegin_ = std::chrono::steady_clock::now();
    audioHighWallNs_ = nowNs();
    cv_.notify_all();
}

void Player::togglePause() {
    if (paused_.load()) play(); else pause();
}

void Player::setVolume(float volume) {
    std::lock_guard<std::mutex> lk(apiMx_);
    volume = std::min(1.0f, std::max(0.0f, volume));
    volume_ = volume;
    if (sink_) sink_->setVolume(volume);
}

void Player::setSpeed(double speed) {
    speed = std::min(4.0, std::max(0.25, speed));
    if (std::abs(speed - speed_.load()) < 1e-6) return;
    pendingSpeed_ = speed;
    speedPending_ = true;
    std::lock_guard lk(mx_);
    cv_.notify_all();
}

void Player::seek(double seconds) {
    seekTarget_ = std::max(0.0, seconds);
    seekPending_ = true;
    cv_.notify_all();
}

bool Player::audioClockActive() const {
    return audioSynced_.load() && audioIndex_.load() >= 0 && sink_ && sink_->isRunning();
}

double Player::masterClock() const {
    // While a seek is in flight the clock is held at the target until there is
    // actually something on screen, otherwise everything decoded afterwards
    // would already be "late".
    if (seeking_.load()) return clockBase_.load();
    // The clock stops where the last frame was shown so the scrub bar stays put.
    if (finished_.load()) return finishClock_.load();

    const int64_t now = nowNs();
    // One second of wall time covers |speed| seconds of media: the sink plays
    // at the device rate, the resampled stream only became shorter/longer.
    const double rate = speed_.load();

    if (audioClockActive()) {
        const double a = clockBase_.load() + sink_->playedSeconds() * rate;
        if (a > audioHigh_.load(std::memory_order_relaxed)) {
            audioHigh_.store(a, std::memory_order_relaxed);
            audioHighWallNs_.store(now, std::memory_order_relaxed);
        }
        if (!demuxDone_.load()) return a;
        if (paused_.load()) return a;
        // At EOF the audio track can end before the video: keep time moving
        // from the last sample we saw instead of freezing the picture.
        const double ext = audioHigh_.load(std::memory_order_relaxed) +
                           static_cast<double>(now - audioHighWallNs_.load()) * 1e-9 * rate;
        return std::max(a, ext);
    }

    const auto tp = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(tp - playStart_).count() - pausedAccum_.load();
    if (paused_.load()) {
        t -= std::chrono::duration<double>(tp - pauseBegin_).count();
    }
    return clockBase_.load() + std::max(0.0, t) * rate;
}

double Player::position() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    const double t = masterClock();
    const double d = duration_.load();
    return d > 0.0 ? std::min(t, d) : t;
}

VideoFramePtr Player::frameForNow() {
    std::lock_guard<std::mutex> lk(apiMx_);

    if (finished_.load()) return current_;

    // Refill the presentation queue; only this thread pushes into it.
    while (displayQueue_.size() < displayQueue_.capacity()) {
        VideoFramePtr f;
        if (!videoQueue_.tryPop(f)) break;
        displayQueue_.push(f);
    }

    if (seeking_.load()) {
        const double target = seekTarget_.load();
        // Safety net for the keyframe mode and for frames queued before the
        // seek target was known; normally primeTo() already did this.
        VideoFramePtr f;
        while (displayQueue_.peek(f) && f->pts + 0.002 < target) displayQueue_.tryPop(f);
        if (displayQueue_.tryPop(f)) {
            {
                std::lock_guard lkc(curMx_);
                current_ = f;
            }
            if (!audioClockActive()) {
                // Video drives the clock: start it on the frame we just showed.
                clockBase_ = f->pts;
                playStart_ = std::chrono::steady_clock::now();
                pausedAccum_ = 0.0;
                seeking_ = false;
            }
        }
        if (seeking_.load() && demuxDone_.load() && displayQueue_.size() == 0 &&
            videoQueue_.size() == 0) {
            seeking_ = false;  // seeked past the end of the file
        }
        return current_;
    }

    const double t = masterClock();
    for (;;) {
        VideoFramePtr f;
        if (!displayQueue_.peek(f)) break;
        if (f->pts > t + 0.002) break;
        displayQueue_.tryPop(f);
        if (current_ && (t - f->pts) > kLateFrameSeconds) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::lock_guard lkc(curMx_);
        current_ = f;
    }

    if (demuxDone_.load() && displayQueue_.size() == 0 && videoQueue_.size() == 0) {
        if (loop_.load()) {
            // Publish the target before the flag; the worker may pick the flag
            // up immediately.
            seekTarget_ = 0.0;
            finished_.store(false);
            seekPending_ = true;
            cv_.notify_all();
        } else {
            finishClock_ = t;
            if (!finished_.exchange(true)) LOGI("eos: finished at %.3fs", t);
        }
    }
    return current_;
}

VideoFramePtr Player::currentFrame() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    std::lock_guard<std::mutex> lkc(curMx_);
    return current_;
}

int Player::width() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->width() : 0;
}

int Player::height() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->height() : 0;
}

int Player::rotation() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->rotation() : 0;
}

bool Player::mirrored() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ && videoDec_->mirrored();
}

bool Player::hardwareDecoding() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ && videoDec_->isHardware();
}

void Player::setHardwarePassthrough(bool on) {
    std::lock_guard<std::mutex> lk(apiMx_);
    if (videoDec_) videoDec_->setPassthrough(on);
}

ID3D11Device* Player::d3dDevice() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->d3dDevice() : nullptr;
}

ID3D11DeviceContext* Player::d3dContext() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->d3dContext() : nullptr;
}

bool Player::d3dLock(void (**lock)(void*), void (**unlock)(void*), void** lockCtx) const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->d3dLock(lock, unlock, lockCtx) : false;
}

void* Player::d3d11DeviceRaw() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return videoDec_ ? videoDec_->d3dDevice() : nullptr;
}

std::vector<Player::AudioTrack> Player::audioTracks() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return audioTracks_;
}

bool Player::selectAudioTrack(int streamIndex) {
    std::lock_guard<std::mutex> lk(apiMx_);
    if (!fmt_ || streamIndex < 0 || streamIndex >= static_cast<int>(fmt_->nb_streams)) return false;
    if (fmt_->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return false;
    pendingAudioIndex_ = streamIndex;
    audioSwitchPending_ = true;
    cv_.notify_all();
    return true;
}

uint64_t Player::audioUnderruns() const {
    std::lock_guard<std::mutex> lk(apiMx_);
    return sink_ ? sink_->underruns() : 0;
}

bool Player::startRecording(const std::string& path, std::string* err) {
    int dw = 0, dh = 0;
    double fps = 30.0;
    bool hasAudio = false;
    int rate = 48000, channels = 2;
    {
        // Only the description of the stream is read under the lock: opening the
        // encoder takes a moment and must not stall the render thread's
        // frameForNow() (it takes the same lock).
        std::lock_guard<std::mutex> lk(apiMx_);
        {
            std::lock_guard<std::mutex> rl(recMx_);
            if (rec_) {
                if (err) *err = "already recording";
                return false;
            }
        }
        if (!videoDec_ || videoIndex_.load() < 0) {
            if (err) *err = "no video stream to record";
            return false;
        }
        const int w = videoDec_->width();
        const int h = videoDec_->height();
        if (w <= 0 || h <= 0) {
            if (err) *err = "the video size is not known yet";
            return false;
        }
        // The file gets the picture the way it is displayed: a 90/270 degree
        // display matrix swaps the dimensions.
        const bool odd = (videoDec_->rotation() & 1) != 0;
        dw = odd ? h : w;
        dh = odd ? w : h;
        const double fd = videoDec_->frameDuration();
        fps = fd > 1e-4 ? 1.0 / fd : 30.0;
        hasAudio = audioDec_ != nullptr && audioIndex_.load() >= 0;
        rate = audioDec_ ? audioDec_->outputSampleRate() : 48000;
        channels = audioDec_ ? audioDec_->outputChannels() : 2;
    }

    auto rec = std::make_shared<Recorder>();
    std::string e;
    if (!rec->start(QString::fromStdString(path), dw, dh, fps, hasAudio, rate, channels, &e)) {
        if (err) *err = e;
        return false;
    }
    {
        // The file may have been closed while the encoder was opening.
        std::lock_guard<std::mutex> lk(apiMx_);
        if (!fmt_ || videoIndex_.load() < 0) {
            if (err) *err = "the file was closed";
            rec->stop();
            return false;
        }
        std::lock_guard<std::mutex> rl(recMx_);
        rec_ = rec;
    }
    LOGI("record: started %s (%dx%d, %.3g fps, audio=%d)", path.c_str(), dw, dh, fps,
         hasAudio ? 1 : 0);
    return true;
}

void Player::stopRecording() {
    std::shared_ptr<Recorder> rec;
    {
        std::lock_guard<std::mutex> rl(recMx_);
        rec.swap(rec_);
    }
    if (!rec) return;
    rec->stop();
    LOGI("record: stopped %s (%llu frames)", qPrintable(rec->path()),
         static_cast<unsigned long long>(rec->videoFrames()));
}

bool Player::isRecording() const {
    std::lock_guard<std::mutex> lk(recMx_);
    return rec_ && rec_->running();
}

std::shared_ptr<Recorder> Player::recorder() const {
    std::lock_guard<std::mutex> lk(recMx_);
    return rec_;
}

void Player::setRecordingLimit(double seconds) {
    if (auto r = recorder()) r->setLimit(seconds);
}

bool Player::recordingLimitReached() const {
    if (auto r = recorder()) return r->limitReached();
    return false;
}
