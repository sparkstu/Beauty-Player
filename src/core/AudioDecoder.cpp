#include "AudioDecoder.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

AudioDecoder::~AudioDecoder() { close(); }

void AudioDecoder::close() {
    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    if (graph_) avfilter_graph_free(&graph_);
    src_ = nullptr;
    sink_ = nullptr;
    pending_.clear();
    if (ctx_) avcodec_free_context(&ctx_);
}

bool AudioDecoder::open(const AVStream* st, std::string* err) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (err) *err = "no audio decoder for stream";
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    avcodec_parameters_to_context(ctx_, st->codecpar);
    ctx_->pkt_timebase = st->time_base;

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        if (err) *err = "avcodec_open2 failed for audio";
        avcodec_free_context(&ctx_);
        return false;
    }

    swr_ = swr_alloc();
    if (!swr_) return false;

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels_);

    av_opt_set_chlayout(swr_, "in_chlayout", &ctx_->ch_layout, 0);
    av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
    av_opt_set_int(swr_, "in_sample_rate", ctx_->sample_rate, 0);
    av_opt_set_int(swr_, "out_sample_rate", outRate_, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", ctx_->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_) < 0) {
        if (err) *err = "swr_init failed";
        close();
        return false;
    }

    LOGI("audio: %d Hz, %d ch, fmt=%s -> %d Hz stereo f32",
         ctx_->sample_rate, ctx_->ch_layout.nb_channels,
         av_get_sample_fmt_name(ctx_->sample_fmt), outRate_);
    return true;
}

bool AudioDecoder::sendPacket(const AVPacket* pkt) {
    if (!ctx_) return false;
    int ret = avcodec_send_packet(ctx_, pkt);
    return ret >= 0 || ret == AVERROR(EAGAIN);
}

// Builds abuffer -> atempo[,...] -> abuffersink. atempo only covers 0.5x..100x,
// so slower rates are a chain of 0.5x stages.
bool AudioDecoder::buildSpeedGraph() {
    if (graph_) avfilter_graph_free(&graph_);
    src_ = nullptr;
    sink_ = nullptr;
    pending_.clear();
    fedSamples_ = 0;
    emittedSamples_ = 0;
    feedBasePts_ = -1.0;

    if (std::abs(speed_ - 1.0) < 1e-6) return true;

    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;

    AVChannelLayout layout;
    av_channel_layout_default(&layout, outChannels_);
    char layoutDesc[64] = {};
    av_channel_layout_describe(&layout, layoutDesc, sizeof(layoutDesc));
    char args[256];
    std::snprintf(args, sizeof(args),
                  "sample_rate=%d:sample_fmt=flt:channel_layout=%s:time_base=1/%d", outRate_,
                  layoutDesc, outRate_);

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !abuffersink ||
        avfilter_graph_create_filter(&src_, abuffer, "in", args, nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&sink_, abuffersink, "out", nullptr, nullptr, graph_) < 0) {
        LOGE("audio: speed graph filters unavailable");
        avfilter_graph_free(&graph_);
        src_ = sink_ = nullptr;
        return false;
    }

    double remaining = speed_;
    char chain[128] = {};
    size_t len = 0;
    auto append = [&](double factor) {
        len += static_cast<size_t>(std::snprintf(chain + len, sizeof(chain) - len, "%satempo=%.4f",
                                                 len ? "," : "", factor));
    };
    while (remaining < 0.5 - 1e-9) {
        append(0.5);
        remaining /= 0.5;
    }
    while (remaining > 2.0 + 1e-9) {
        append(2.0);
        remaining /= 2.0;
    }
    if (std::abs(remaining - 1.0) > 1e-3) append(remaining);

    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;
    const int parsed = avfilter_graph_parse_ptr(graph_, chain, &inputs, &outputs, nullptr);
    if (parsed < 0 || !inputs || !outputs ||
        avfilter_link(src_, 0, inputs->filter_ctx, inputs->pad_idx) < 0 ||
        avfilter_link(outputs->filter_ctx, outputs->pad_idx, sink_, 0) < 0) {
        LOGE("audio: cannot build the atempo chain (%s)", chain);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&graph_);
        src_ = sink_ = nullptr;
        return false;
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (avfilter_graph_config(graph_, nullptr) < 0) {
        LOGE("audio: speed graph config failed");
        avfilter_graph_free(&graph_);
        src_ = sink_ = nullptr;
        return false;
    }
    lastChain_ = chain;
    return true;
}

void AudioDecoder::setSpeed(double speed) {
    speed = std::min(4.0, std::max(0.25, speed));
    if (std::abs(speed - speed_) < 1e-6) return;
    speed_ = speed;
    // The graph is rebuilt here and again by the flush that follows the
    // re-seek; both are cheap and it keeps the state machine simple.
    buildSpeedGraph();
    LOGI("audio: speed %.2fx (%s)", speed_,
         graph_ ? lastChain_.c_str() : "bypass");
}

void AudioDecoder::feedSpeedGraph(const float* interleaved, int frames, double pts) {
    if (!graph_ || !src_ || frames <= 0) return;
    if (feedBasePts_ < 0.0) feedBasePts_ = pts;

    AVFrame* in = av_frame_alloc();
    if (!in) return;
    in->format = AV_SAMPLE_FMT_FLT;
    in->sample_rate = outRate_;
    av_channel_layout_default(&in->ch_layout, outChannels_);
    in->nb_samples = frames;
    in->pts = fedSamples_;
    if (av_frame_get_buffer(in, 0) < 0) {
        av_frame_free(&in);
        return;
    }
    std::memcpy(in->data[0], interleaved,
                static_cast<size_t>(frames) * outChannels_ * sizeof(float));
    fedSamples_ += frames;

    if (av_buffersrc_add_frame_flags(src_, in, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
        LOGW("audio: buffersrc rejected a frame");
    av_frame_free(&in);
}

void AudioDecoder::drainSpeedGraph() {
    if (!graph_ || !sink_) return;
    for (;;) {
        AVFrame* f = av_frame_alloc();
        if (!f) return;
        if (av_buffersink_get_frame(sink_, f) < 0) {
            av_frame_free(&f);
            return;
        }
        const int n = f->nb_samples;
        const int ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : outChannels_;
        Chunk c;
        c.pcm.resize(static_cast<size_t>(n) * outChannels_);
        if (ch == outChannels_) {
            std::memcpy(c.pcm.data(), f->data[0],
                        static_cast<size_t>(n) * outChannels_ * sizeof(float));
        } else {
            std::fill(c.pcm.begin(), c.pcm.end(), 0.0f);
        }
        // atempo output is a continuous stream; one output sample covers
        // |speed_| input samples, so the media time advances accordingly.
        c.pts = feedBasePts_ < 0.0 ? 0.0
                                   : feedBasePts_ + static_cast<double>(emittedSamples_) * speed_ /
                                                          outRate_;
        emittedSamples_ += n;
        pending_.push_back(std::move(c));
        av_frame_free(&f);
    }
}

int AudioDecoder::receive(std::vector<float>& out, double* ptsOut) {
    if (!ctx_) return 0;

    for (;;) {
        if (!pending_.empty()) {
            Chunk c = std::move(pending_.front());
            pending_.pop_front();
            out = std::move(c.pcm);
            if (ptsOut) *ptsOut = c.pts;
            return static_cast<int>(out.size() / static_cast<size_t>(outChannels_));
        }

        AVFrame* f = av_frame_alloc();
        if (!f) return 0;
        if (avcodec_receive_frame(ctx_, f) < 0) {
            av_frame_free(&f);
            return 0;
        }

        double pts = lastPts_;
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts != AV_NOPTS_VALUE) {
            lastPts_ = ts * av_q2d(ctx_->pkt_timebase);
            pts = lastPts_;
        } else {
            lastPts_ += static_cast<double>(f->nb_samples) / outRate_;
            pts = lastPts_;
        }

        int maxOut = swr_get_out_samples(swr_, f->nb_samples);
        if (maxOut <= 0) maxOut = f->nb_samples + 256;

        std::vector<float> tmp(static_cast<size_t>(maxOut) * outChannels_);
        uint8_t* outPtr[1] = { reinterpret_cast<uint8_t*>(tmp.data()) };
        int got = swr_convert(swr_, outPtr, maxOut,
                              const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        av_frame_free(&f);
        if (got <= 0) continue;

        if (!graph_) {
            tmp.resize(static_cast<size_t>(got) * outChannels_);
            out = std::move(tmp);
            if (ptsOut) *ptsOut = pts;
            return got;
        }

        feedSpeedGraph(tmp.data(), got, pts);
        drainSpeedGraph();
        // atempo buffers a little; with nothing produced yet, decode the next
        // frame and try again.
    }
}

void AudioDecoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
    if (swr_) swr_convert(swr_, nullptr, 0, nullptr, 0);
    lastPts_ = 0.0;
    // atempo keeps state across seeks; rebuilding also drops half-used chunks.
    buildSpeedGraph();
}
