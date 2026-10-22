#include "core/Recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include <QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "core/Log.h"

namespace {

std::string errStr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// Bitrate from the pixel rate. 0.09 bit per pixel keeps a 1080p30 recording
// around 5.6 Mbit/s and 4K60 around 45 Mbit/s, which is visually transparent
// for desktop content and leaves the encoder plenty of headroom.
int64_t pickBitrate(int w, int h, double fps) {
    const double rate = (fps > 1.0 && fps < 240.0) ? fps : 30.0;
    int64_t bitrate = int64_t(double(w) * double(h) * rate * 0.09);
    bitrate = std::clamp<int64_t>(bitrate, 4'000'000, 80'000'000);
    const int mbps = qEnvironmentVariableIntValue("PLAY_REC_BITRATE");
    if (mbps > 0) bitrate = int64_t(mbps) * 1'000'000;
    return bitrate;
}

// Formats the encoder can take, preferring NV12 (what the hardware encoders
// want) and falling back to planar YUV420P (libopenh264). A NULL list means the
// codec accepts anything.
AVPixelFormat pickPixelFormat(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs,
                                     &count) < 0 ||
        !configs)
        return AV_PIX_FMT_YUV420P;
    const auto* list = static_cast<const AVPixelFormat*>(configs);
    AVPixelFormat planar = AV_PIX_FMT_NONE;
    for (int i = 0; i < count; ++i) {
        if (list[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
        if (list[i] == AV_PIX_FMT_YUV420P && planar == AV_PIX_FMT_NONE) planar = list[i];
    }
    return planar == AV_PIX_FMT_NONE ? AV_PIX_FMT_YUV420P : planar;
}

}  // namespace

Recorder::~Recorder() {
    stop();
}

bool Recorder::start(const QString& path, int width, int height, double fps, bool hasAudio,
                     int sampleRate, int channels, std::string* err) {
    if (running_.load()) {
        if (err) *err = "already recording";
        return false;
    }
    // Encoders want even dimensions; the display size can be odd after a
    // rotation, so round down instead of failing.
    width_ = std::max(2, width & ~1);
    height_ = std::max(2, height & ~1);
    if (width < 2 || height < 2) {
        if (err) *err = "no video to record";
        return false;
    }
    path_ = path;
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    channels_ = channels > 0 ? channels : 2;

    int r = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", qPrintable(path));
    if (!fmt_ || r < 0) {
        if (err) *err = "cannot create the muxer for " + path.toStdString();
        closeAll();
        return false;
    }

    if (!openVideo(width_, height_, fps, err)) {
        closeAll();
        return false;
    }
    if (hasAudio && !openAudio(sampleRate_, channels_, err)) {
        // A missing AAC encoder should not cost the video: log it and continue
        // with a video-only file.
        LOGW("record: no audio track (%s)", err ? err->c_str() : "?");
        if (err) err->clear();
        ast_ = nullptr;
    }

    r = avio_open(&fmt_->pb, qPrintable(path), AVIO_FLAG_WRITE);
    if (r < 0) {
        if (err) *err = "cannot open " + path.toStdString() + ": " + errStr(r);
        closeAll();
        return false;
    }
    // Fragmented MP4: the index is written as we go instead of only in the
    // trailer, so a crash or a hard kill leaves a playable file (a plain MP4
    // without its moov is unreadable) and the keyframe interval bounds the loss.
    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    r = avformat_write_header(fmt_, &muxOpts);
    av_dict_free(&muxOpts);
    if (r < 0) {
        if (err) *err = "cannot write the header: " + errStr(r);
        closeAll();
        return false;
    }

    stopping_ = false;
    failed_ = false;
    videoFrames_ = 0;
    audioFrames_ = 0;
    dropped_ = 0;
    bytes_ = 0;
    t0_ = -1.0;
    shift_ = 0.0;
    lastVideoOut_ = -1.0;
    lastAudioOut_ = -1.0;
    cuts_ = 0;
    audioPending_.clear();
    lastLogMs_ = 0;
    {
        std::lock_guard<std::mutex> lk(mx_);
        videoQ_.clear();
        audioQ_.clear();
    }
    running_ = true;
    worker_ = std::thread([this] { worker(); });

    LOGI("record: %s %dx%d %.3g fps, %s + %s, %lld kbit/s", qPrintable(path), width_, height_,
         fps, qPrintable(codec_), ast_ ? "aac" : "no audio", static_cast<long long>(venc_->bit_rate / 1000));
    return true;
}

bool Recorder::openVideo(int w, int h, double fps, std::string* err) {
    const double rate = (fps > 1.0 && fps < 240.0) ? fps : 30.0;
    frameDur_ = 1.0 / rate;
    const int64_t bitrate = pickBitrate(w, h, rate);

    // Hardware first: NVENC, then Quick Sync, then AMF, then the LGPL software
    // encoder. avcodec_find_encoder_by_name() succeeds even when the machine has
    // no such hardware, so the winner is the first one that actually opens.
    QStringList candidates;
    const QString forced = qEnvironmentVariable("PLAY_REC_CODEC");
    if (!forced.isEmpty()) {
        candidates << forced;
    } else {
        candidates << QStringLiteral("h264_nvenc") << QStringLiteral("h264_qsv")
                   << QStringLiteral("h264_amf") << QStringLiteral("libopenh264");
    }

    std::string lastError = "no H.264 encoder available";
    for (const QString& name : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.toLatin1().constData());
        if (!codec) {
            lastError = name.toStdString() + ": encoder not built in";
            continue;
        }
        AVCodecContext* enc = avcodec_alloc_context3(codec);
        if (!enc) continue;
        enc->width = w;
        enc->height = h;
        enc->time_base = AVRational{1, 90000};
        enc->framerate = AVRational{int(std::lround(rate * 1000.0)), 1000};
        enc->pix_fmt = pickPixelFormat(codec);
        enc->bit_rate = bitrate;
        enc->rc_max_rate = bitrate;
        enc->rc_buffer_size = int(bitrate * 2);
        enc->gop_size = int(std::lround(rate));
        enc->max_b_frames = 2;
        enc->color_range = AVCOL_RANGE_MPEG;
        enc->color_primaries = AVCOL_PRI_BT709;
        enc->color_trc = AVCOL_TRC_BT709;
        enc->colorspace = AVCOL_SPC_BT709;
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER)
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (name == QStringLiteral("h264_nvenc")) {
            av_opt_set(enc->priv_data, "preset", "p5", 0);
            av_opt_set(enc->priv_data, "tune", "hq", 0);
            av_opt_set(enc->priv_data, "rc", "vbr", 0);
        } else if (name == QStringLiteral("libopenh264")) {
            av_opt_set(enc->priv_data, "profile", "high", 0);
            av_opt_set(enc->priv_data, "rc_mode", "bitrate", 0);
        }

        const int r = avcodec_open2(enc, codec, nullptr);
        if (r < 0) {
            lastError = name.toStdString() + ": " + errStr(r);
            avcodec_free_context(&enc);
            continue;
        }
        venc_ = enc;
        codec_ = QString::fromLatin1(codec->name);
        break;
    }
    if (!venc_) {
        if (err) *err = lastError;
        return false;
    }

    vst_ = avformat_new_stream(fmt_, nullptr);
    if (!vst_) {
        if (err) *err = "cannot add the video stream";
        return false;
    }
    vst_->time_base = venc_->time_base;
    int r = avcodec_parameters_from_context(vst_->codecpar, venc_);
    if (r < 0) {
        if (err) *err = "cannot copy the video parameters: " + errStr(r);
        return false;
    }

    sws_ = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, venc_->pix_fmt, SWS_BILINEAR, nullptr,
                          nullptr, nullptr);
    if (!sws_) {
        if (err) *err = "cannot create the RGBA converter";
        return false;
    }
    // The readback is sRGB (full range); the file is BT.709 limited range, the
    // combination every player expects for HD H.264.
    const int* coeff = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws_, coeff, 0, coeff, 1, 0, 1 << 16, 1 << 16);

    vframe_ = av_frame_alloc();
    if (!vframe_) {
        if (err) *err = "cannot allocate a video frame";
        return false;
    }
    vframe_->format = venc_->pix_fmt;
    vframe_->width = w;
    vframe_->height = h;
    r = av_frame_get_buffer(vframe_, 0);
    if (r < 0) {
        if (err) *err = "cannot allocate the video buffer: " + errStr(r);
        return false;
    }
    return true;
}

bool Recorder::openAudio(int rate, int channels, std::string* err) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        if (err) *err = "no AAC encoder";
        return false;
    }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) {
        if (err) *err = "cannot allocate the audio encoder";
        return false;
    }
    enc->sample_rate = rate;
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&enc->ch_layout, std::clamp(channels, 1, 2));
    enc->bit_rate = 192'000;
    enc->time_base = AVRational{1, rate};
    if (fmt_->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int r = avcodec_open2(enc, codec, nullptr);
    if (r < 0) {
        if (err) *err = "cannot open AAC: " + errStr(r);
        avcodec_free_context(&enc);
        return false;
    }
    aenc_ = enc;

    ast_ = avformat_new_stream(fmt_, nullptr);
    if (!ast_) {
        if (err) *err = "cannot add the audio stream";
        return false;
    }
    ast_->time_base = aenc_->time_base;
    r = avcodec_parameters_from_context(ast_->codecpar, aenc_);
    if (r < 0) {
        if (err) *err = "cannot copy the audio parameters: " + errStr(r);
        return false;
    }

    aframe_ = av_frame_alloc();
    if (!aframe_) {
        if (err) *err = "cannot allocate an audio frame";
        return false;
    }
    aframe_->format = aenc_->sample_fmt;
    aframe_->sample_rate = aenc_->sample_rate;
    av_channel_layout_copy(&aframe_->ch_layout, &aenc_->ch_layout);
    aframe_->nb_samples = aenc_->frame_size > 0 ? aenc_->frame_size : 1024;
    r = av_frame_get_buffer(aframe_, 0);
    if (r < 0) {
        if (err) *err = "cannot allocate the audio buffer: " + errStr(r);
        return false;
    }
    return true;
}

void Recorder::stop() {
    if (!running_.load() && !worker_.joinable()) {
        closeAll();
        return;
    }
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lk(mx_);
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    running_ = false;
    closeAll();
    logStats();
}

void Recorder::logStats() {
    if (videoFrames_.load() == 0 && audioFrames_.load() == 0) return;
    LOGI("record: wrote %llu video / %llu audio frames, %lld KiB, %llu dropped%s",
         static_cast<unsigned long long>(videoFrames_.load()),
         static_cast<unsigned long long>(audioFrames_.load()),
         static_cast<long long>(bytes_.load() / 1024),
         static_cast<unsigned long long>(dropped_.load()), failed_.load() ? ", FAILED" : "");
}

void Recorder::closeAll() {
    if (fmt_ && fmt_->pb) {
        avio_closep(&fmt_->pb);
    }
    av_frame_free(&vframe_);
    av_frame_free(&aframe_);
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    avcodec_free_context(&venc_);
    avcodec_free_context(&aenc_);
    vst_ = nullptr;
    ast_ = nullptr;
    if (fmt_) avformat_free_context(fmt_);
    fmt_ = nullptr;
}

void Recorder::pushVideo(QByteArray rgba, int width, int height, double pts) {
    if (!running_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed))
        return;
    // The encoder wants even dimensions, so a rotated odd-sized video can be one
    // pixel wider/taller than the file. sws crops the extra row and column.
    if (width < width_ || height < height_ || width - width_ > 1 || height - height_ > 1) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGW("record: dropping a %dx%d frame, the file is %dx%d", width, height, width_,
                 height_);
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (rgba.size() < qsizetype(width) * height * 4) return;

    std::unique_lock<std::mutex> lk(mx_);
    // Keep the queue short: a slow encoder must cost frames, not memory (a 4K
    // RGBA frame is 33 MB).
    const size_t maxFrames = std::max<size_t>(2, (size_t(192) << 20) / (size_t(width) * height * 4));
    if (videoQ_.size() >= maxFrames && backpressure_.load(std::memory_order_relaxed)) {
        // Offline export: losing a frame would leave a hole in the file, so wait
        // for the encoder instead (the exporter is not racing a clock).
        while (running_.load(std::memory_order_relaxed)
               && !stopping_.load(std::memory_order_relaxed) && videoQ_.size() >= maxFrames) {
            cvSpace_.wait_for(lk, std::chrono::milliseconds(20));
        }
    }
    if (videoQ_.size() >= maxFrames) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    videoQ_.push_back(VideoJob{ std::move(rgba), width, height, pts });
    cv_.notify_one();
}

void Recorder::pushAudio(const float* pcm, int frames, double pts) {
    if (!running_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed))
        return;
    if (!ast_ || !pcm || frames <= 0) return;
    std::vector<float> copy(pcm, pcm + size_t(frames) * size_t(channels_));
    std::lock_guard<std::mutex> lk(mx_);
    // ~4 seconds is plenty: the video queue bounds the lead anyway.
    if (audioQ_.size() >= 200) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    audioQ_.emplace_back(std::move(copy), pts);
    cv_.notify_one();
}

void Recorder::setLimit(double seconds) {
    limit_.store(seconds, std::memory_order_relaxed);
}

void Recorder::worker() {
    LOGI("record: encoder thread started");
    for (;;) {
        VideoJob job;
        bool haveVideo = false;
        std::vector<std::pair<std::vector<float>, double>> audioBatch;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return stopping_.load() || !videoQ_.empty() || !audioQ_.empty();
            });
            if (!videoQ_.empty()) {
                job = std::move(videoQ_.front());
                videoQ_.pop_front();
                haveVideo = true;
            }
            // An offline export hands over audio far faster than one block per
            // video frame, so take a batch; in real time there is only one.
            for (int i = 0; i < (haveVideo ? 32 : 1) && !audioQ_.empty(); ++i) {
                audioBatch.emplace_back(std::move(audioQ_.front().first), audioQ_.front().second);
                audioQ_.pop_front();
            }
            if (!haveVideo && audioBatch.empty() && stopping_.load()) break;
        }
        if (haveVideo) {
            encodeVideo(job);
            cvSpace_.notify_all();
        }
        for (auto& a : audioBatch) queueAudio(std::move(a.first), a.second);

        const int64_t nowMs = int64_t(logElapsedMs());
        if (nowMs - lastLogMs_ > 5000) {
            lastLogMs_ = nowMs;
            LOGI("record: %llu frames, %.1f MiB%s", static_cast<unsigned long long>(videoFrames_.load()),
                 double(bytes_.load()) / (1024.0 * 1024.0),
                 dropped_.load() ? qPrintable(QStringLiteral(", %1 dropped").arg(dropped_.load()))
                                 : "");
        }
    }

    // Drain what the queues still hold, then flush both encoders.
    for (;;) {
        VideoJob job;
        std::vector<float> audio;
        double audioPts = 0.0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (videoQ_.empty() && audioQ_.empty()) break;
            if (!videoQ_.empty()) {
                job = std::move(videoQ_.front());
                videoQ_.pop_front();
            } else {
                audio = std::move(audioQ_.front().first);
                audioPts = audioQ_.front().second;
                audioQ_.pop_front();
            }
        }
        if (job.width > 0) {
            encodeVideo(job);
        } else {
            queueAudio(std::move(audio), audioPts);
        }
    }
    flushEncoders();

    if (fmt_ && fmt_->pb) {
        const int r = av_write_trailer(fmt_);
        if (r < 0) LOGE("record: trailer: %s", errStr(r).c_str());
        avio_closep(&fmt_->pb);
    }
    running_ = false;
    LOGI("record: encoder thread finished");
}

double Recorder::mapTime(double pts, double expected, double lastOut) {
    double out = pts - t0_ - shift_;
    // Backwards by more than half a second: a seek or a loop restarted the media
    // clock (the start-up skew between the two streams is far smaller, and a
    // duplicate timestamp is smaller still). Forwards: only a jump longer than
    // 5 s is folded; a shorter one is a real stall and stays a frozen frame.
    if (out < expected - 0.5 || out > expected + 5.0) {
        shift_ += out - expected;
        out = expected;
        ++cuts_;
        LOGI("record: timeline jump folded into a cut (%llu)",
             static_cast<unsigned long long>(cuts_));
        return out;
    }
    // Before the recording started, or a timestamp the file already has (the
    // catch-up burst right after a seek): skip it. Clamping these into the
    // timeline would compress the clip by a few percent.
    if (out < 0.0) return -1.0;
    if (lastOut >= 0.0 && out <= lastOut) return -1.0;
    return out;
}

void Recorder::encodeVideo(VideoJob& job) {
    if (!venc_ || !vst_ || failed_.load()) return;
    if (t0_ < 0.0) t0_ = job.pts;

    const double expected = lastVideoOut_ < 0.0 ? 0.0 : lastVideoOut_ + frameDur_;
    const double rel = mapTime(job.pts, expected, lastVideoOut_);
    if (rel < 0.0) {
        // Before the start, or a moment the file already covers (the catch-up
        // burst after a seek): skipping beats repeating a frame at +1 ms.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const double lim = limit_.load(std::memory_order_relaxed);
    if (lim >= 0.0 && rel >= lim) {
        limitHit_.store(true, std::memory_order_relaxed);
        return;
    }
    lastVideoOut_ = rel;

    if (av_frame_make_writable(vframe_) < 0) return;
    const uint8_t* src[4] = { reinterpret_cast<const uint8_t*>(job.rgba.constData()), nullptr,
                              nullptr, nullptr };
    const int srcStride[4] = { job.width * 4, 0, 0, 0 };
    sws_scale(sws_, src, srcStride, 0, job.height, vframe_->data, vframe_->linesize);

    vframe_->pts = int64_t(std::llround(rel * 90000.0));

    const int r = avcodec_send_frame(venc_, vframe_);
    if (r < 0) {
        LOGE("record: send video: %s", errStr(r).c_str());
        failed_ = true;
        return;
    }
    drainEncoder(venc_, vst_);
    videoFrames_.fetch_add(1, std::memory_order_relaxed);
}

void Recorder::queueAudio(std::vector<float> pcm, double pts) {
    if (!aenc_ || !ast_ || failed_.load() || pcm.empty()) return;
    if (t0_ < 0.0) t0_ = pts;
    const size_t frameSize = size_t(channels_);
    // The audio thread runs ahead of the first displayed frame, so the first
    // few blocks can predate the recording. Drop them: writing a negative pts
    // makes the muxer insert an empty edit (0.2 s of dead air at the start).
    const double blockDur = double(pcm.size() / frameSize) / double(sampleRate_);
    if (pts + blockDur <= t0_ + 1e-6) return;

    // A seek (or a track switch) leaves a hole: finish the partial block so the
    // file stays continuous, then restart the timestamp from the new chunk.
    if (!audioPending_.empty()) {
        const double pendingEnd =
            audioPendingPts_ + double(audioPending_.size() / frameSize) / double(sampleRate_);
        if (std::fabs(pts - pendingEnd) > 0.05) {
            const size_t missing = (frameSize * size_t(aenc_->frame_size > 0 ? aenc_->frame_size : 1024))
                                   - audioPending_.size();
            audioPending_.insert(audioPending_.end(), missing, 0.f);
            encodeAudioBlock(audioPending_.data(), int(audioPending_.size() / frameSize),
                             audioPendingPts_);
            audioPending_.clear();
            audioPendingPts_ = pts;
        }
    }
    if (audioPending_.empty()) audioPendingPts_ = pts;
    audioPending_.insert(audioPending_.end(), pcm.begin(), pcm.end());

    const size_t blockFrames = size_t(aenc_->frame_size > 0 ? aenc_->frame_size : 1024);
    size_t offset = 0;
    while (audioPending_.size() - offset >= blockFrames * frameSize) {
        const double blockPts =
            audioPendingPts_ + double(offset / frameSize) / double(sampleRate_);
        encodeAudioBlock(audioPending_.data() + offset, int(blockFrames), blockPts);
        offset += blockFrames * frameSize;
    }
    if (offset > 0) {
        audioPending_.erase(audioPending_.begin(), audioPending_.begin() + offset);
        // The leftover samples start where the encoded blocks ended, otherwise
        // the next block would be stamped in the past and the muxer rejects it.
        audioPendingPts_ += double(offset / frameSize) / double(sampleRate_);
    }
}

void Recorder::encodeAudioBlock(const float* pcm, int frames, double pts) {
    if (!aenc_ || !ast_ || frames <= 0) return;
    if (av_frame_make_writable(aframe_) < 0) return;
    aframe_->nb_samples = frames;
    const int ch = channels_;
    // Interleaved float -> planar float, which is what AAC takes.
    for (int c = 0; c < ch; ++c) {
        auto* dst = reinterpret_cast<float*>(aframe_->data[c]);
        for (int i = 0; i < frames; ++i) dst[i] = pcm[size_t(i) * size_t(ch) + size_t(c)];
    }
    const double expected =
        lastAudioOut_ < 0.0 ? 0.0 : lastAudioOut_ + double(frames) / double(sampleRate_);
    const double rel = mapTime(pts, expected, lastAudioOut_);
    if (rel < 0.0) return;  // predates the recording: nothing to write
    const double lim = limit_.load(std::memory_order_relaxed);
    if (lim >= 0.0 && rel >= lim) return;  // past the clip end
    lastAudioOut_ = rel;
    aframe_->pts = int64_t(std::llround(rel * double(sampleRate_)));

    const int r = avcodec_send_frame(aenc_, aframe_);
    if (r < 0) {
        LOGE("record: send audio: %s", errStr(r).c_str());
        failed_ = true;
        return;
    }
    drainEncoder(aenc_, ast_);
    audioFrames_.fetch_add(uint64_t(frames), std::memory_order_relaxed);
}

void Recorder::drainEncoder(AVCodecContext* enc, AVStream* st) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    for (;;) {
        const int r = avcodec_receive_packet(enc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) {
            LOGE("record: receive packet: %s", errStr(r).c_str());
            failed_ = true;
            break;
        }
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        // av_interleaved_write_frame() takes ownership and blanks the packet,
        // so the size has to be read first.
        const int size = pkt->size;
        const int w = av_interleaved_write_frame(fmt_, pkt);
        if (w < 0) {
            LOGE("record: write packet: %s", errStr(w).c_str());
            failed_ = true;
        } else {
            bytes_.fetch_add(size, std::memory_order_relaxed);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void Recorder::flushEncoders() {
    if (aenc_ && ast_ && !audioPending_.empty()) {
        const size_t blockFrames = size_t(aenc_->frame_size > 0 ? aenc_->frame_size : 1024);
        const size_t have = audioPending_.size() / size_t(channels_);
        if (have > 0) {
            // Pad the last block with silence so the tail is not cut off.
            if (have < blockFrames)
                audioPending_.insert(audioPending_.end(),
                                     (blockFrames - have) * size_t(channels_), 0.f);
            encodeAudioBlock(audioPending_.data(), int(blockFrames), audioPendingPts_);
        }
        audioPending_.clear();
    }
    if (venc_ && vst_) {
        avcodec_send_frame(venc_, nullptr);
        drainEncoder(venc_, vst_);
    }
    if (aenc_ && ast_) {
        avcodec_send_frame(aenc_, nullptr);
        drainEncoder(aenc_, ast_);
    }
}
