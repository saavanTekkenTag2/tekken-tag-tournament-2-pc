// Android audio output: replaces audio_output.cpp (XAudio2) with one AAudio stream.
//
// Same contract as the PC version (audio_output.h): each guest render driver is a client that
// submits 256-sample frames of 6-channel float PCM (FL FR FC LFE BL BR) and is paced by render
// credits - one per frame the device has finished playing, AUDIO_OUTPUT_PRIME_FRAMES up front.
// XAudio2 gave every client its own source voice; here a single 48 kHz stereo AAudio stream
// pulls from every started client in its data callback and mixes them down to stereo.
// Without a device (AAudio unavailable or the stream fails), clients fall back to the PC
// version's silent 48 kHz clock, so the game keeps its timing.
#include "audio_output.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/log.h>
#endif

AudioPcmConversion ConvertAudioPcm(const uint8_t* guest, std::array<float, AUDIO_PCM_VALUES>& interleaved)
{
    AudioPcmConversion result;
    for (size_t sample = 0; sample < AUDIO_PCM_SAMPLES; ++sample)
        for (size_t channel = 0; channel < AUDIO_PCM_CHANNELS; ++channel)
        {
            const uint8_t* p = guest + (channel * AUDIO_PCM_SAMPLES + sample) * 4;
            const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            float value = std::bit_cast<float>(bits);
            if (!std::isfinite(value)) { value = 0; ++result.nonfinite; }
            result.nonzero |= value != 0;
            interleaved[sample * AUDIO_PCM_CHANNELS + channel] = value;
        }
    return result;
}

namespace {
struct RenderWake
{
    std::mutex mutex;
    std::condition_variable event;
    uint64_t version = 0;
};
RenderWake& RenderSignal() { static auto* s = new RenderWake; return *s; }
void NotifyRender()
{
    auto& s = RenderSignal();
    { std::lock_guard lock(s.mutex); ++s.version; }
    s.event.notify_all();
}

enum class FramePhase { Free, Queued };
struct OutputFrame
{
    std::array<float, AUDIO_PCM_VALUES> samples{};
    FramePhase phase = FramePhase::Free;
};
} // namespace

struct AudioOutputState
{
    explicit AudioOutputState(uint32_t id) : driver(id) {}
    mutable std::mutex mutex;
    const uint32_t driver;
    bool closing = false;
    uint32_t renderCredits = AUDIO_OUTPUT_PRIME_FRAMES;
    uint64_t fallbackNextNs = 0;
    std::array<OutputFrame, AUDIO_OUTPUT_QUEUE_FRAMES> frames;
    std::deque<size_t> queue;          // frames in play order
    size_t playOffset = 0;             // samples of queue.front() already played
    AudioOutputStats stats;
    std::atomic<uint64_t> maxPassGapUs{ 0 };
    std::atomic<uint32_t> latePasses{ 0 };
};

namespace {
constexpr int kRate = 48000;

class Mixer
{
public:
    static Mixer& Get() { static auto* m = new Mixer(); return *m; }

    void Add(const std::shared_ptr<AudioOutputState>& s)
    {
        std::lock_guard lock(mutex_);
        clients_.push_back(s);
        if (!ok_)
        {
            std::lock_guard sl(s->mutex);
            s->stats.failed = true;
        }
        else
        {
            std::lock_guard sl(s->mutex);
            s->stats.ready = true;
        }
    }
    void Remove(const AudioOutputState* s)
    {
        std::lock_guard lock(mutex_);
        std::erase_if(clients_, [s](const auto& c) { return c.get() == s; });
    }
    std::atomic<uint32_t> glitches{ 0 }, latency{ 0 };

private:
    Mixer()
    {
        if (const char* e = std::getenv("TTT2_AUDIO_SILENT"); e && *e == '1')
        {
            std::fprintf(stderr, "[snd-output] TTT2_AUDIO_SILENT=1: no device, silent callback pacing\n");
            return;
        }
#ifdef __ANDROID__
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { std::fprintf(stderr, "[snd-output] AAudio builder failed; silent pacing\n"); return; }
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setSampleRate(b, kRate);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(b, &Mixer::Callback, this);
        AAudioStreamBuilder_setErrorCallback(b, &Mixer::OnError, this);
        const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK || !stream_) { std::fprintf(stderr, "[snd-output] AAudio open failed (%d); silent pacing\n", int(r)); stream_ = nullptr; return; }
        rate_ = AAudioStream_getSampleRate(stream_);
        // Two bursts of buffer: low latency without underruns on most devices.
        const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
        AAudioStream_setBufferSizeInFrames(stream_, std::max(burst * 2, 256));
        if (AAudioStream_requestStart(stream_) != AAUDIO_OK) { AAudioStream_close(stream_); stream_ = nullptr; std::fprintf(stderr, "[snd-output] AAudio start failed; silent pacing\n"); return; }
        ok_ = true;
        std::fprintf(stderr, "[snd-output] AAudio ready: %d Hz float stereo, burst %d, buffer %d frames; game mixes 6ch -> stereo\n",
                     rate_, burst, AAudioStream_getBufferSizeInFrames(stream_));
        if (rate_ != kRate) std::fprintf(stderr, "[snd-output] device runs at %d Hz: AAudio resamples\n", rate_);
#else
        std::fprintf(stderr, "[snd-output] no AAudio on this build; silent callback pacing\n");
#endif
    }

#ifdef __ANDROID__
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audioData, int32_t numFrames)
    {
        static_cast<Mixer*>(user)->Mix(static_cast<float*>(audioData), numFrames);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    static void OnError(AAudioStream*, void* user, aaudio_result_t error)
    {
        std::fprintf(stderr, "[snd-output] AAudio error %d (device change?): reopening\n", int(error));
        // Reopen from another thread (not allowed inside the callback).
        std::thread([m = static_cast<Mixer*>(user)] { m->Reopen(); }).detach();
    }
    void Reopen()
    {
        std::lock_guard lock(reopenMutex_);
        if (stream_) { AAudioStream_requestStop(stream_); AAudioStream_close(stream_); stream_ = nullptr; }
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setSampleRate(b, kRate);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setDataCallback(b, &Mixer::Callback, this);
        AAudioStreamBuilder_setErrorCallback(b, &Mixer::OnError, this);
        if (AAudioStreamBuilder_openStream(b, &stream_) == AAUDIO_OK && stream_) AAudioStream_requestStart(stream_);
        AAudioStreamBuilder_delete(b);
    }
#endif

    // 6ch -> stereo (ITU downmix; LFE dropped)
    static void Downmix(const float* s, float& l, float& r)
    {
        constexpr float c = 0.70710678f;
        l += s[0] + c * s[2] + c * s[4];
        r += s[1] + c * s[2] + c * s[5];
    }

    void Mix(float* out, int32_t frames)
    {
        std::fill(out, out + size_t(frames) * 2, 0.0f);
        const auto now = std::chrono::steady_clock::now();
        const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        std::vector<std::shared_ptr<AudioOutputState>> clients;
        {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) { ++glitches; return; }
            clients = clients_;
        }
        bool freed = false;
        for (auto& sp : clients)
        {
            AudioOutputState& s = *sp;
            std::lock_guard lock(s.mutex);
            if (s.closing) continue;
            // pass gap statistics (as XAudio2's OnVoiceProcessingPassStart)
            if (lastPassNs_)
            {
                const uint64_t gapUs = uint64_t(nowNs - lastPassNs_) / 1000;
                uint64_t longest = s.maxPassGapUs.load(std::memory_order_relaxed);
                while (gapUs > longest && !s.maxPassGapUs.compare_exchange_weak(longest, gapUs)) {}
                if (gapUs > 20000) s.latePasses.fetch_add(1, std::memory_order_relaxed);
            }
            if (!s.stats.started)
            {
                if (s.queue.size() >= AUDIO_OUTPUT_PRIME_FRAMES) s.stats.started = true;
                else continue;
            }
            int32_t done = 0;
            while (done < frames)
            {
                if (s.queue.empty())
                {
                    ++s.stats.starvationPasses;
                    s.stats.starvationBytes += uint64_t(frames - done) * AUDIO_PCM_CHANNELS * sizeof(float);
                    break;
                }
                OutputFrame& f = s.frames[s.queue.front()];
                const size_t avail = AUDIO_PCM_SAMPLES - s.playOffset;
                const size_t take = std::min<size_t>(avail, size_t(frames - done));
                for (size_t i = 0; i < take; ++i)
                    Downmix(&f.samples[(s.playOffset + i) * AUDIO_PCM_CHANNELS], out[(done + i) * 2], out[(done + i) * 2 + 1]);
                s.playOffset += take;
                done += int32_t(take);
                if (s.playOffset == AUDIO_PCM_SAMPLES)
                {
                    f.phase = FramePhase::Free;
                    s.queue.pop_front();
                    s.playOffset = 0;
                    --s.stats.queued;
                    if (s.stats.deviceQueued) --s.stats.deviceQueued;
                    ++s.stats.completed;
                    s.renderCredits = std::min<uint32_t>(s.renderCredits + 1, AUDIO_OUTPUT_PRIME_FRAMES);
                    freed = true;
                }
            }
        }
        lastPassNs_ = nowNs;
        for (int32_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        if (freed) NotifyRender();
    }

    std::mutex mutex_;
    std::mutex reopenMutex_;
    std::vector<std::shared_ptr<AudioOutputState>> clients_;
    bool ok_ = false;
    int rate_ = kRate;
    int64_t lastPassNs_ = 0;
#ifdef __ANDROID__
    AAudioStream* stream_ = nullptr;
#endif
};
} // namespace

uint64_t AudioOutputWakeVersion()
{
    auto& s = RenderSignal();
    std::lock_guard lock(s.mutex);
    return s.version;
}

void WaitAudioOutputWake(uint64_t version, uint64_t timeoutNs)
{
    auto& s = RenderSignal();
    std::unique_lock lock(s.mutex);
    s.event.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [&] { return s.version != version; });
}

AudioOutputClient::AudioOutputClient(uint32_t driver) : state_(std::make_shared<AudioOutputState>(driver))
{
    Mixer::Get().Add(state_);
    NotifyRender();
}

AudioOutputClient::~AudioOutputClient() { Close(); }

void AudioOutputClient::Close()
{
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closing) return;
        state_->closing = true;
        state_->queue.clear();
        for (auto& f : state_->frames) if (f.phase != FramePhase::Free) { f.phase = FramePhase::Free; ++state_->stats.dropped; }
        state_->stats.queued = state_->stats.deviceQueued = 0;
        state_->stats.ready = state_->stats.started = false;
    }
    Mixer::Get().Remove(state_.get());
}

void AudioOutputClient::Submit(const uint8_t* guest)
{
    std::lock_guard lock(state_->mutex);
    ++state_->stats.received;
    if (state_->closing || state_->stats.failed) { ++state_->stats.dropped; return; }
    auto free = std::find_if(state_->frames.begin(), state_->frames.end(), [](const auto& f) { return f.phase == FramePhase::Free; });
    if (free == state_->frames.end()) { ++state_->stats.dropped; return; }
    const auto conversion = ConvertAudioPcm(guest, free->samples);
    state_->stats.nonzero += conversion.nonzero;
    state_->stats.nonfinite += conversion.nonfinite;
    free->phase = FramePhase::Queued;
    state_->queue.push_back(size_t(free - state_->frames.begin()));
    ++state_->stats.queued;
    ++state_->stats.deviceQueued;
    ++state_->stats.submitted;
}

bool AudioOutputClient::TryAcquireRenderCredit(uint64_t nowNs)
{
    std::lock_guard lock(state_->mutex);
    if (state_->closing) return false;
    if (state_->stats.failed)
    {
        if (nowNs < state_->fallbackNextNs) return false;
        state_->fallbackNextNs = nowNs + AUDIO_PCM_FRAME_NS;
        return true;
    }
    if (!state_->renderCredits) return false;
    --state_->renderCredits;
    return true;
}

uint32_t AudioOutputClient::Queued() const
{
    std::lock_guard lock(state_->mutex);
    return state_->stats.queued;
}

AudioOutputStats AudioOutputClient::Stats() const
{
    std::lock_guard lock(state_->mutex);
    auto r = state_->stats;
    r.renderCredits = state_->renderCredits;
    r.engineGlitches = Mixer::Get().glitches.load(std::memory_order_relaxed);
    r.engineLatency = Mixer::Get().latency.load(std::memory_order_relaxed);
    r.maxPassGapUs = state_->maxPassGapUs.exchange(0, std::memory_order_relaxed);
    r.latePasses = state_->latePasses.exchange(0, std::memory_order_relaxed);
    return r;
}
