#pragma once

/*
 * 实时系统输出采集的跨平台接口。
 *
 * 头文件刻意不包含 RtAudio.h：main.cpp 只需知道“开始、停止、查询状态”，
 * 不需要了解第三方库细节。这种把实现藏在 Impl 中的写法叫 PImpl
 * （Pointer to Implementation），还能减少头文件变化引起的大量重新编译。
 */

#include <cstddef>
#include <memory>
#include <string>

class LoopbackCapture {
public:
	/*
	 * 音频数据回调类型。
	 *
	 * userdata    调用者自己的上下文指针，本项目传入 Vis*；
	 * monoSamples 单声道 float 数组，只在本次回调期间有效；
	 * sampleCount 数组中的采样数量，不是字节数量。
	 */
    using SampleCallback = void (*)(void* userdata, const float* monoSamples,
                                    std::size_t sampleCount);

    LoopbackCapture();
    ~LoopbackCapture();

	// 音频流持有系统资源，复制两个实例容易导致重复释放，因此明确禁止复制。
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

	// 成功返回 true；失败返回 false，并把适合显示的原因写入 error。
    bool Start(SampleCallback callback, void* userdata, std::string& error);
	// 可重复调用；未运行时调用也安全。
    void Stop();
    bool IsRunning() const;

private:
	// Impl 的完整定义只出现在 .cpp 中；unique_ptr 负责自动释放它。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
