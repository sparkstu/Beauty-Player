#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

// Audio decoder + resampler. Always outputs interleaved float32, 48 kHz, stereo,
// which is what miniaudio is configured for.
//
// Speed changes go through an avfilter graph (atempo, pitch preserved) that
// sits after the resampler. At 1x the graph does not exist, so normal playback
// pays nothing for the feature.
class AudioDecoder {
public:
    ~AudioDecoder();

    bool open(const AVStream* st, std::string* err);
    void close();

    bool sendPacket(const AVPacket* pkt);
    // Fills |out| with interleaved samples; returns the number of frames.
    // |ptsOut| (optional) receives the media time of the first sample.
    int receive(std::vector<float>& out, double* ptsOut = nullptr);
    void flush();

    // 0.25x .. 4x. Only the worker thread may call this.
    void setSpeed(double speed);
    double speed() const { return speed_; }

    int outputSampleRate() const { return outRate_; }
    int outputChannels() const { return outChannels_; }

private:
    struct Chunk {
        std::vector<float> pcm;
        double pts = 0.0;
    };

    bool buildSpeedGraph();
    void feedSpeedGraph(const float* interleaved, int frames, double pts);
    void drainSpeedGraph();

    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    int outRate_ = 48000;
    int outChannels_ = 2;
    double lastPts_ = 0.0;

    // atempo chain (only when speed_ != 1).
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    double speed_ = 1.0;
    double feedBasePts_ = -1.0;  // media pts of the first fed sample since flush
    int64_t fedSamples_ = 0;
    int64_t emittedSamples_ = 0;
    std::deque<Chunk> pending_;
    std::string lastChain_;  // for the log line only
};
