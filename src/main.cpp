#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>              // 没有 backends/ 前缀了
#include <imgui_impl_sdlrenderer3.h>
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

static constexpr int FFT_N     = 2048;
static constexpr int BAR_COUNT = 80;

struct Vis {
    std::mutex mu;
    std::vector<float> in;
    float mag[BAR_COUNT]{};
    float peak[BAR_COUNT]{};
};

static void SDLCALL PostMix(void* userdata, const SDL_AudioSpec* spec,
                            float* buffer, int buflen)
{
    auto* v = static_cast<Vis*>(userdata);
    int frames = buflen / spec->channels;
    std::lock_guard<std::mutex> lock(v->mu);
    v->in.reserve(v->in.size() + frames);
    for (int i = 0; i < frames; i++) {
        float s = 0.0f;
        for (int c = 0; c < spec->channels; c++) s += buffer[i * spec->channels + c];
        v->in.push_back(s / (float)spec->channels);
    }
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
    for (int i = 0; i < FFT_N; i++) tmp[i] = v.in[i] * window[i];
    v.in.erase(v.in.begin(), v.in.begin() + FFT_N / 2);

    std::vector<kiss_fft_cpx> out(FFT_N / 2 + 1);
    kiss_fftr(cfg, tmp.data(), out.data());

    for (int b = 0; b < BAR_COUNT; b++) {
        float lo = powf((b + 0.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
        float hi = powf((b + 1.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
        int i0 = std::max(1, (int)lo), i1 = std::min((int)hi, FFT_N / 2);
        float sum = 0.0f;
        for (int i = i0; i < i1; i++) sum += out[i].r * out[i].r + out[i].i * out[i].i;
        float db = 10.0f * log10f(sum / (float)(i1 - i0) + 1e-9f);
        float val = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);
        v.mag[b]  += (val - v.mag[b]) * 0.35f;
        v.peak[b]  = (val > v.peak[b]) ? val : v.peak[b] * 0.97f;
    }
}

static bool LoadAndPlay(const char* path, SDL_AudioDeviceID& dev,
                        SDL_AudioStream*& stream, Vis& vis)
{
    if (stream) { SDL_DestroyAudioStream(stream); stream = nullptr; }
    SDL_AudioSpec spec;
    Uint8* data = nullptr; Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &data, &len)) {
        SDL_Log("load wav fail: %s", SDL_GetError());
        return false;
    }
    if (!dev) dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    stream = SDL_OpenAudioDeviceStream(dev, &spec, nullptr, nullptr);
    SDL_SetAudioPostmixCallback(dev, PostMix, &vis);   // 挂设备，不是流
    SDL_PutAudioStreamData(stream, data, (int)len);
    SDL_ResumeAudioStream(stream);
    SDL_ResumeAudioDevice(dev);
    SDL_free(data);
    return true;
}

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { SDL_Log("%s", SDL_GetError()); return 1; }

    SDL_Window* win = SDL_CreateWindow("Audio Visualizer", 1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    ImGui::StyleColorsDark();

    SDL_AudioDeviceID dev = 0;
    SDL_AudioStream* stream = nullptr;
    Vis vis;
    char pathBuf[512] = "assets/test.wav";

    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
        }
        ComputeFFT(vis);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Control");
        ImGui::InputText("WAV path", pathBuf, sizeof(pathBuf));
        if (ImGui::Button("Play")) LoadAndPlay(pathBuf, dev, stream, vis);
        ImGui::SameLine(); if (ImGui::Button("Pause")) SDL_PauseAudioStream(stream);
        ImGui::SameLine(); if (ImGui::Button("Resume")) SDL_ResumeAudioStream(stream);
        ImGui::Text("FPS %.1f", io.Framerate);
        ImGui::End();

        ImGui::Begin("Spectrum");
        ImVec2 cv = ImGui::GetContentRegionAvail();
        ImDrawList* d = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = cv.x / BAR_COUNT, h = cv.y;
        for (int i = 0; i < BAR_COUNT; i++) {
            ImU32 col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(0.1f + vis.mag[i]*0.5f, 0.4f + vis.mag[i]*0.6f, 1.0f, 1.0f));
            float bh = vis.mag[i] * h * 0.9f;
            d->AddRectFilled(ImVec2(p0.x+i*w+1, p0.y+h-bh),
                             ImVec2(p0.x+(i+1)*w-1, p0.y+h), col, 2.0f);
            float ph = vis.peak[i] * h * 0.9f;
            d->AddRectFilled(ImVec2(p0.x+i*w+1, p0.y+h-ph-3),
                             ImVec2(p0.x+(i+1)*w-1, p0.y+h-ph), 0xFFFFFFFF, 1.0f);
        }
        ImGui::Dummy(cv);
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 18, 18, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }

    ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyAudioStream(stream); SDL_CloseAudioDevice(dev);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
}