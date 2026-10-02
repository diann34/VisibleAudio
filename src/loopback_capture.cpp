#include "loopback_capture.h"

#include <RtAudio.h>
#include <SDL3/SDL_log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool LooksLikeLoopbackDevice(const std::string& name)
{
    const std::string lower = Lowercase(name);
    static constexpr const char* keywords[] = {
        "monitor", "loopback", "blackhole", "soundflower",
        "stereo mix", "what u hear", "wave out"
    };
    for (const char* keyword : keywords) {
        if (lower.find(keyword) != std::string::npos) return true;
    }
    return false;
}

RtAudio::Api PreferredApi()
{
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    for (RtAudio::Api api : apis) {
        if (Lowercase(RtAudio::getApiName(api)).find("pulse") != std::string::npos)
            return api;
    }
    return RtAudio::UNSPECIFIED;
}

} // namespace

struct LoopbackCapture::Impl {
    std::unique_ptr<RtAudio> audio;
    SampleCallback callback = nullptr;
    void* userdata = nullptr;
    unsigned int channels = 0;
    std::atomic<bool> overflowReported{false};
    std::vector<float> mono;

    static int AudioCallback(void*, void* inputBuffer, unsigned int frameCount,
                             double, RtAudioStreamStatus status, void* userdata)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (status != 0 && !self->overflowReported.exchange(true))
            SDL_Log("real-time audio capture overflow detected");
        if (!inputBuffer || !self->callback || self->channels == 0) return 0;

        const auto* input = static_cast<const float*>(inputBuffer);
        self->mono.resize(frameCount);
        for (unsigned int frame = 0; frame < frameCount; ++frame) {
            float sum = 0.0f;
            for (unsigned int channel = 0; channel < self->channels; ++channel)
                sum += input[static_cast<std::size_t>(frame) * self->channels + channel];
            self->mono[frame] = sum / static_cast<float>(self->channels);
        }
        self->callback(self->userdata, self->mono.data(), self->mono.size());
        return 0;
    }

    bool TryOpen(unsigned int deviceId, const RtAudio::DeviceInfo& info,
                 bool allowOutputLoopback, std::string& error)
    {
        unsigned int availableChannels = info.inputChannels;
        if (availableChannels == 0 && allowOutputLoopback)
            availableChannels = info.outputChannels;
        if (availableChannels == 0) return false;

        channels = std::min(availableChannels, 2U);
        unsigned int sampleRate = info.preferredSampleRate;
        if (sampleRate == 0) sampleRate = 48000;
        unsigned int bufferFrames = 512;
        RtAudio::StreamParameters inputParameters;
        inputParameters.deviceId = deviceId;
        inputParameters.nChannels = channels;
        inputParameters.firstChannel = 0;

        RtAudioErrorType result = audio->openStream(nullptr, &inputParameters,
            RTAUDIO_FLOAT32, sampleRate, &bufferFrames, AudioCallback, this);
        if (result != RTAUDIO_NO_ERROR) {
            error = audio->getErrorText();
            return false;
        }
        result = audio->startStream();
        if (result != RTAUDIO_NO_ERROR) {
            error = audio->getErrorText();
            audio->closeStream();
            return false;
        }

        SDL_Log("capturing real-time output: %s (%u Hz, %u channels)",
            info.name.c_str(), sampleRate, channels);
        return true;
    }
};

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}

LoopbackCapture::~LoopbackCapture()
{
    Stop();
}

bool LoopbackCapture::Start(SampleCallback callback, void* userdata, std::string& error)
{
    Stop();
    impl_->callback = callback;
    impl_->userdata = userdata;
    impl_->overflowReported.store(false);
    impl_->audio = std::make_unique<RtAudio>(PreferredApi());

    const std::vector<unsigned int> ids = impl_->audio->getDeviceIds();
    if (ids.empty()) {
        error = "RtAudio did not find any audio devices";
        impl_->audio.reset();
        return false;
    }

    struct Candidate {
        unsigned int id;
        RtAudio::DeviceInfo info;
        bool outputLoopback;
    };
    std::vector<Candidate> candidates;

    // PulseAudio/PipeWire monitor sources and virtual loopback drivers on
    // CoreAudio appear as regular input devices, so prefer those everywhere.
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info = impl_->audio->getDeviceInfo(id);
        if (info.inputChannels > 0 && LooksLikeLoopbackDevice(info.name))
            candidates.push_back({id, std::move(info), false});
    }

    // RtAudio's WASAPI backend treats an output-only device opened for input
    // as loopback. Other backends reject this cleanly, after which the error
    // below explains how to expose a monitor/virtual loopback input.
    const unsigned int defaultOutput = impl_->audio->getDefaultOutputDevice();
    if (defaultOutput != 0) {
        const bool alreadyAdded = std::any_of(candidates.begin(), candidates.end(),
            [defaultOutput](const Candidate& item) { return item.id == defaultOutput; });
        if (!alreadyAdded) {
            RtAudio::DeviceInfo info = impl_->audio->getDeviceInfo(defaultOutput);
            if (info.outputChannels > 0)
                candidates.push_back({defaultOutput, std::move(info), true});
        }
    }

    std::string lastError;
    for (const Candidate& candidate : candidates) {
        if (impl_->TryOpen(candidate.id, candidate.info,
                           candidate.outputLoopback, lastError)) return true;
        if (impl_->audio->isStreamOpen()) impl_->audio->closeStream();
    }

    error = "No usable system-output loopback device was found";
    if (!lastError.empty()) error += ": " + lastError;
    error += ". On Linux select a PulseAudio/PipeWire monitor; on macOS "
             "install a virtual loopback device such as BlackHole.";
    impl_->audio.reset();
    return false;
}

void LoopbackCapture::Stop()
{
    if (!impl_->audio) return;
    if (impl_->audio->isStreamRunning()) impl_->audio->stopStream();
    if (impl_->audio->isStreamOpen()) impl_->audio->closeStream();
    impl_->audio.reset();
}

bool LoopbackCapture::IsRunning() const
{
    return impl_->audio && impl_->audio->isStreamRunning();
}
