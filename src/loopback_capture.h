#pragma once

#include <cstddef>
#include <memory>
#include <string>

class LoopbackCapture {
public:
    using SampleCallback = void (*)(void* userdata, const float* monoSamples,
                                    std::size_t sampleCount);

    LoopbackCapture();
    ~LoopbackCapture();

    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

    bool Start(SampleCallback callback, void* userdata, std::string& error);
    void Stop();
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
