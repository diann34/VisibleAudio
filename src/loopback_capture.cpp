#include "loopback_capture.h"

/*
 * 本文件只调用 RtAudio 的统一接口，不直接包含 WASAPI、PulseAudio 或 CoreAudio
 * 头文件。具体平台后端由 vcpkg 构建的 RtAudio 选择：
 *   Windows -> WASAPI loopback
 *   Linux   -> PulseAudio/PipeWire monitor
 *   macOS   -> CoreAudio 中可见的虚拟回采设备（如 BlackHole）
 */

#include <RtAudio.h>
#include <SDL3/SDL_log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace {

// 设备名匹配不应区分大小写，先统一转换成小写。
std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool LooksLikeLoopbackDevice(const std::string& name)
{
    const std::string lower = Lowercase(name);
	// 不同系统/驱动对“系统输出回采”的命名不同，覆盖常见名称。
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
	// Linux 上 ALSA 通常只看到硬件设备，而 PulseAudio/PipeWire 会暴露 monitor，
	// 所以编译了 Pulse 后优先选它。其他平台让 RtAudio 使用默认后端。
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
	// RtAudio 对象拥有底层设备和流。unique_ptr 让 Stop() 能明确地销毁/重建它。
    std::unique_ptr<RtAudio> audio;
    SampleCallback callback = nullptr;
    void* userdata = nullptr;
    unsigned int channels = 0;
    std::atomic<bool> overflowReported{false};
	// 重用单声道缓冲，避免每次回调都重新申请一块内存。
	std::vector<float> mono;

	/*
	 * RtAudio 的实时线程回调。输入通常是交错排列：
	 *   L0, R0, L1, R1, L2, R2, ...
	 * 本函数把同一帧的所有声道求平均，输出：M0, M1, M2, ...
	 */
    static int AudioCallback(void*, void* inputBuffer, unsigned int frameCount,
                             double, RtAudioStreamStatus status, void* userdata)
    {
        auto* self = static_cast<Impl*>(userdata);
		// exchange(true) 既设置标志又返回旧值，保证溢出日志只打印一次。
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
		// monitor/虚拟设备本来就有 inputChannels；Windows 默认扬声器则由
		// RtAudio 以 outputChannels 创建 WASAPI loopback 输入流。
        unsigned int availableChannels = info.inputChannels;
        if (availableChannels == 0 && allowOutputLoopback)
            availableChannels = info.outputChannels;
        if (availableChannels == 0) return false;

		// 可视化无需保留 5.1/7.1，最多读取两个声道后混成单声道即可。
		channels = std::min(availableChannels, 2U);
        unsigned int sampleRate = info.preferredSampleRate;
        if (sampleRate == 0) sampleRate = 48000;
		// 512 只是期望值，openStream 可能按设备能力修改它。
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
	// RAII：对象离开作用域时自动停止采集，不把音频设备泄漏给操作系统。
    Stop();
}

bool LoopbackCapture::Start(SampleCallback callback, void* userdata, std::string& error)
{
	// 允许用户反复点击 Start：先关掉旧流，再建立一个干净的新流。
    Stop();
    impl_->callback = callback;
    impl_->userdata = userdata;
    impl_->overflowReported.store(false);
    impl_->audio = std::make_unique<RtAudio>(PreferredApi());

	// RtAudio 的设备 ID 是不透明整数，只能拿来继续调用 RtAudio。
	const std::vector<unsigned int> ids = impl_->audio->getDeviceIds();
    if (ids.empty()) {
        error = "RtAudio did not find any audio devices";
        impl_->audio.reset();
        return false;
    }

	// 先收集候选，再按优先顺序逐个尝试；某个坏驱动失败不会阻止后续候选。
	struct Candidate {
        unsigned int id;
        RtAudio::DeviceInfo info;
        bool outputLoopback;
    };
    std::vector<Candidate> candidates;

	// PulseAudio/PipeWire monitor 和 CoreAudio 虚拟回采设备表现为普通输入设备，
	// 所以在所有平台上优先尝试名称明确的 loopback 设备。
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info = impl_->audio->getDeviceInfo(id);
        if (info.inputChannels > 0 && LooksLikeLoopbackDevice(info.name))
            candidates.push_back({id, std::move(info), false});
    }

	// RtAudio 的 WASAPI 后端会把“以输入方式打开的输出设备”解释为 loopback。
	// 其他后端会正常返回失败，随后错误信息会提示如何提供 monitor/虚拟设备。
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
	// 顺序很重要：先停止回调，再关闭流，最后销毁 RtAudio 对象。
    if (!impl_->audio) return;
    if (impl_->audio->isStreamRunning()) impl_->audio->stopStream();
    if (impl_->audio->isStreamOpen()) impl_->audio->closeStream();
    impl_->audio.reset();
}

bool LoopbackCapture::IsRunning() const
{
    return impl_->audio && impl_->audio->isStreamRunning();
}
