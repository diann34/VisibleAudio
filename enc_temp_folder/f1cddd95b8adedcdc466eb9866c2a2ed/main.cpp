#include "loopback_capture.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>              // 没有 backends/ 前缀了
#include <imgui_impl_sdlrenderer3.h>
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

static constexpr int FFT_N     = 2048;
static constexpr int BAR_COUNT = 80;
static constexpr int MAX_PENDING_SAMPLES = FFT_N * 8;

struct Vis {
    std::mutex mu;
    std::vector<float> in;
    float mag[BAR_COUNT]{};
    float peak[BAR_COUNT]{};
};

struct AppLog {
    std::mutex mu;
    std::deque<std::string> lines;
    SDL_LogOutputFunction forward = nullptr;
    void* forwardUserdata = nullptr;
};

static const char* LogPriorityName(SDL_LogPriority priority)
{
    switch (priority) {
    case SDL_LOG_PRIORITY_TRACE:    return "TRACE";
    case SDL_LOG_PRIORITY_VERBOSE:  return "VERBOSE";
    case SDL_LOG_PRIORITY_DEBUG:    return "DEBUG";
    case SDL_LOG_PRIORITY_INFO:     return "INFO";
    case SDL_LOG_PRIORITY_WARN:     return "WARN";
    case SDL_LOG_PRIORITY_ERROR:    return "ERROR";
    case SDL_LOG_PRIORITY_CRITICAL: return "CRITICAL";
    default:                        return "LOG";
    }
}

static void SDLCALL CaptureLog(void* userdata, int category,
                               SDL_LogPriority priority, const char* message)
{
    auto* log = static_cast<AppLog*>(userdata);
    std::string line = "[";
    line += LogPriorityName(priority);
    line += "] ";
    line += message ? message : "";

    {
        std::lock_guard<std::mutex> lock(log->mu);
        log->lines.push_back(std::move(line));
        while (log->lines.size() > 200) log->lines.pop_front();
    }

    // Keep SDL's normal debugger/console output in addition to the UI copy.
    if (log->forward) log->forward(log->forwardUserdata, category, priority, message);
}

static void DrawLogBackground(ImDrawList* drawList, ImVec2 min, ImVec2 size,
                              AppLog& log)
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(log.mu);
        lines.assign(log.lines.begin(), log.lines.end());
    }

    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const int visibleLines = std::max(0, (int)((size.y - 36.0f) / lineHeight));
    const int first = std::max(0, (int)lines.size() - visibleLines);
    ImVec2 max(min.x + size.x, min.y + size.y);
    drawList->PushClipRect(min, max, true);
    drawList->AddText(ImVec2(min.x + 8.0f, min.y + 7.0f),
        IM_COL32(150, 165, 190, 70), "PROGRAM LOG");
    float y = min.y + 27.0f;
    for (int i = first; i < (int)lines.size(); ++i, y += lineHeight) {
        drawList->AddText(ImVec2(min.x + 8.0f, y),
            IM_COL32(185, 195, 215, 55), lines[i].c_str());
    }
    drawList->PopClipRect();
}

static void SDLCALL PostMix(void* userdata, const SDL_AudioSpec* spec,
                            float* buffer, int buflen)
{
    auto* v = static_cast<Vis*>(userdata);
    int frames = buflen / (int)(spec->channels * sizeof(float));
    std::lock_guard<std::mutex> lock(v->mu);
    v->in.reserve(v->in.size() + frames);
    for (int i = 0; i < frames; i++) {
        float s = 0.0f;
        for (int c = 0; c < spec->channels; c++) s += buffer[i * spec->channels + c];
        v->in.push_back(s / (float)spec->channels);
    }
    if ((int)v->in.size() > MAX_PENDING_SAMPLES)
        v->in.erase(v->in.begin(), v->in.end() - MAX_PENDING_SAMPLES);
}

static void ReceiveRealtimeSamples(void* userdata, const float* samples,
                                   std::size_t sampleCount)
{
    auto* v = static_cast<Vis*>(userdata);
    std::lock_guard<std::mutex> lock(v->mu);
    v->in.insert(v->in.end(), samples, samples + sampleCount);
    if ((int)v->in.size() > MAX_PENDING_SAMPLES)
        v->in.erase(v->in.begin(), v->in.end() - MAX_PENDING_SAMPLES);
}

static void ClearVisualization(Vis& v)
{
    std::lock_guard<std::mutex> lock(v.mu);
    v.in.clear();
    std::fill(std::begin(v.mag), std::end(v.mag), 0.0f);
    std::fill(std::begin(v.peak), std::end(v.peak), 0.0f);
}

static void ComputeFFT(Vis& v)
{
    std::lock_guard<std::mutex> lock(v.mu);
    if ((int)v.in.size() < FFT_N) return;

    static kiss_fftr_cfg cfg = kiss_fftr_alloc(FFT_N, 0, nullptr, nullptr);
    static std::vector<kiss_fft_scalar> tmp(FFT_N);
    static std::vector<float> window(FFT_N);
    static bool once = false;
    if (!once) {
        for (int i = 0; i < FFT_N; i++)
            window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_N - 1)));
        once = true;
    }
    // Always analyze the newest samples so a low UI frame rate cannot make
    // the real-time visualization lag behind the captured output.
    auto newest = v.in.end() - FFT_N;
    for (int i = 0; i < FFT_N; i++) tmp[i] = newest[i] * window[i];
    v.in.erase(v.in.begin(), v.in.end() - FFT_N / 2);

    std::vector<kiss_fft_cpx> out(FFT_N / 2 + 1);
    kiss_fftr(cfg, tmp.data(), out.data());

    for (int b = 0; b < BAR_COUNT; b++) {
        float lo = powf((b + 0.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
        float hi = powf((b + 1.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
        int i0 = std::clamp((int)floorf(lo), 1, FFT_N / 2);
        int i1 = std::clamp((int)ceilf(hi), i0 + 1, FFT_N / 2 + 1);
        float sum = 0.0f;
        for (int i = i0; i < i1; i++) sum += out[i].r * out[i].r + out[i].i * out[i].i;
        // kissfft does not normalize its output. Normalize before converting to dB,
        // otherwise almost every non-silent bin is clamped to a full-height bar.
        float magnitude = sqrtf(sum / (float)(i1 - i0)) * (4.0f / FFT_N);
        float db = 20.0f * log10f(magnitude + 1e-9f);
        float val = std::clamp((db + 70.0f) / 70.0f, 0.0f, 1.0f);
        v.mag[b]  += (val - v.mag[b]) * 0.35f;
        v.peak[b]  = (val > v.peak[b]) ? val : v.peak[b] * 0.97f;
    }
}

static bool LoadAndPlay(const char* path, SDL_AudioDeviceID& dev,
                        SDL_AudioStream*& stream, Vis& vis)
{
    if (stream) {
        // A stream created by SDL_OpenAudioDeviceStream owns its logical device.
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
        dev = 0;
    }
    SDL_AudioSpec spec;
    Uint8* data = nullptr; Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &data, &len)) {
        SDL_Log("load wav fail: %s", SDL_GetError());
        return false;
    }
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, nullptr, nullptr);
    if (!stream) { SDL_Log("open stream fail: %s", SDL_GetError()); SDL_free(data); return false; }

    dev = SDL_GetAudioStreamDevice(stream);
    if (!dev || !SDL_SetAudioPostmixCallback(dev, PostMix, &vis)) {
        SDL_Log("set postmix callback fail: %s", SDL_GetError());
        SDL_free(data);
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
        dev = 0;
        return false;
    }

    ClearVisualization(vis);

    bool queued = SDL_PutAudioStreamData(stream, data, (int)len);
    SDL_free(data);
    if (!queued || !SDL_ResumeAudioStreamDevice(stream)) {
        SDL_Log("start playback fail: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
        dev = 0;
        return false;
    }
    SDL_Log("playing WAV: %s (%d Hz, %d channels, %u bytes)",
        path, spec.freq, (int)spec.channels, (unsigned)len);
    return true;
}

int main(int, char**)
{
    AppLog appLog;
    SDL_GetLogOutputFunction(&appLog.forward, &appLog.forwardUserdata);
    SDL_SetLogOutputFunction(CaptureLog, &appLog);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL initialization failed: %s", SDL_GetError());
        SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("Audio Visualizer", 1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    //io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    ImGui::StyleColorsDark();

    SDL_AudioDeviceID dev = 0;
    SDL_AudioStream* stream = nullptr;
    Vis vis;
    LoopbackCapture loopback;
    char pathBuf[512] = "assets/test.wav";
    int sourceMode = 0;
    bool limitFps = true;
    int targetFps = 60;
    SDL_Log("VisibleAudio started");

    bool run = true;
    while (run) {
        const Uint64 frameStart = SDL_GetTicksNS();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
        }
        ComputeFFT(vis);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 workPos = viewport->WorkPos;
        const ImVec2 workSize = viewport->WorkSize;
        const float controlHeight = std::min(140.0f, workSize.y);
        const float fpsPanelWidth = std::min(300.0f, workSize.x * 0.32f);
        const float controlWidth = std::max(1.0f, workSize.x - fpsPanelWidth);
        constexpr ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings;

        // Explicitly lay out both panels. In particular, do not reuse an old
        // imgui.ini entry that may have saved Spectrum as a 32x35 window.
        ImGui::SetNextWindowPos(workPos);
        ImGui::SetNextWindowSize(ImVec2(controlWidth, controlHeight));
        ImGui::Begin("Control", nullptr, panelFlags);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##Audio source", &sourceMode,
                         "WAV file\0System output (live)\0")) {
            if (stream) {
                SDL_DestroyAudioStream(stream);
                stream = nullptr;
                dev = 0;
            }
            loopback.Stop();
            ClearVisualization(vis);
            SDL_Log("audio source changed to %s",
                sourceMode == 0 ? "WAV file" : "system output");
        }

        if (sourceMode == 0) {
            ImGui::TextUnformatted("WAV path");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##WAV path", pathBuf, sizeof(pathBuf));
            if (ImGui::Button("Play")) LoadAndPlay(pathBuf, dev, stream, vis);
            ImGui::SameLine();
            if (ImGui::Button("Pause") && stream) {
                if (SDL_PauseAudioStreamDevice(stream)) SDL_Log("playback paused");
                else SDL_Log("pause playback failed: %s", SDL_GetError());
            }
            ImGui::SameLine();
            if (ImGui::Button("Resume") && stream) {
                if (SDL_ResumeAudioStreamDevice(stream)) SDL_Log("playback resumed");
                else SDL_Log("resume playback failed: %s", SDL_GetError());
            }
        } else {
            if (!loopback.IsRunning()) {
                if (ImGui::Button("Start capture")) {
                    ClearVisualization(vis);
                    std::string error;
                    if (!loopback.Start(ReceiveRealtimeSamples, &vis, error))
                        SDL_Log("start real-time output capture failed: %s", error.c_str());
                }
            } else if (ImGui::Button("Stop capture")) {
                loopback.Stop();
                SDL_Log("real-time output capture stopped");
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(loopback.IsRunning() ? "Capturing" : "Stopped");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(workPos.x + controlWidth, workPos.y));
        ImGui::SetNextWindowSize(ImVec2(fpsPanelWidth, controlHeight));
        ImGui::Begin("FPS Limit", nullptr, panelFlags);
        if (ImGui::Checkbox("Enabled", &limitFps))
            SDL_Log("FPS limit %s", limitFps ? "enabled" : "disabled");
        ImGui::BeginDisabled(!limitFps);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##Target FPS", &targetFps, 15, 240, "%d FPS"))
            targetFps = std::clamp(targetFps, 15, 240);
        if (ImGui::IsItemDeactivatedAfterEdit())
            SDL_Log("FPS target set to %d", targetFps);
        ImGui::EndDisabled();
        ImGui::Text("Actual: %.1f FPS", io.Framerate);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(workPos.x, workPos.y + controlHeight));
        ImGui::SetNextWindowSize(ImVec2(workSize.x, std::max(1.0f, workSize.y - controlHeight)));
        ImGui::Begin("Spectrum", nullptr, panelFlags);
        ImVec2 cv = ImGui::GetContentRegionAvail();
        ImDrawList* d = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = cv.x / BAR_COUNT, h = cv.y;
        float pad = std::min(1.0f, w * 0.15f);
        DrawLogBackground(d, p0, cv, appLog);
        for (int i = 0; i < BAR_COUNT; i++) {
            ImU32 col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(0.1f + vis.mag[i]*0.5f, 0.4f + vis.mag[i]*0.6f, 1.0f, 1.0f));
            float bh = vis.mag[i] * h * 0.9f;
            d->AddRectFilled(ImVec2(p0.x+i*w+pad, p0.y+h-bh),
                             ImVec2(p0.x+(i+1)*w-pad, p0.y+h), col, 2.0f);
            float ph = vis.peak[i] * h * 0.9f;
            d->AddRectFilled(ImVec2(p0.x+i*w+pad, p0.y+h-ph-3),
                             ImVec2(p0.x+(i+1)*w-pad, p0.y+h-ph), 0xFFFFFFFF, 1.0f);
        }
        ImGui::Dummy(cv);
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 18, 18, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);

        if (limitFps) {
            const Uint64 targetFrameTime = 1000000000ULL / (Uint64)targetFps;
            const Uint64 elapsed = SDL_GetTicksNS() - frameStart;
            if (elapsed < targetFrameTime) SDL_DelayPrecise(targetFrameTime - elapsed);
        }
    }

    loopback.Stop();
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (stream) SDL_DestroyAudioStream(stream);
    SDL_Log("VisibleAudio stopped");
    SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
