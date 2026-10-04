/*
 * VisibleAudio 的程序入口与可视化主流程。
 *
 * 可以把本文件理解成 4 个连续阶段：
 *   1. 音频回调把立体声/多声道采样混合为单声道，写入 Vis::in；
 *   2. ComputeFFT() 把最近一段时域采样转换成频域能量；
 *   3. ImGui 根据 Vis::mag / Vis::peak 画出频谱柱和峰值线；
 *   4. SDL Renderer 把 ImGui 生成的图形真正提交给显卡。
 *
 * 音频回调运行在后台线程，界面和 FFT 运行在主线程。因此，两个线程共同
 * 访问 Vis::in 时必须持有 Vis::mu，避免一个线程读 vector 时另一个线程
 * 改变它的大小（这会造成数据竞争，严重时会崩溃）。
 */
#include "loopback_capture.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h> // CMake 已把 extern/imgui/backends 加入头文件搜索路径
#include <imgui_impl_sdlrenderer3.h>
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// 一次 FFT 使用的单声道采样数。值越大，频率分辨率越高，但响应延迟也越大。
static constexpr int FFT_N = 2048;
// 屏幕上绘制的频谱柱数量，并不等于 FFT 输出频点数量。
static constexpr int BAR_COUNT = 80;
// 限制后台线程积压的数据量，防止低帧率时 vector 无限增长。
static constexpr int MAX_PENDING_SAMPLES = FFT_N * 8;

// 可视化所需的共享状态。
struct Vis {
	// mu（mutex，互斥锁）保护下面三个成员的跨线程访问。
	std::mutex mu;
	// 等待 FFT 处理的单声道浮点采样；正常范围约为 [-1.0, 1.0]。
	std::vector<float> in;
	// 每根柱当前的平滑高度，范围 [0, 1]。
	float mag[BAR_COUNT]{};
	// 每根柱的峰值线高度，范围 [0, 1]。
	float peak[BAR_COUNT]{};
};

// 保存最近的程序日志。SDL 日志可能来自任意线程，所以也需要互斥锁。
struct AppLog {
	std::mutex mu;
	std::deque<std::string> lines;
	// 安装自定义日志回调前的 SDL 回调。转发给它可保留控制台/调试器输出。
	SDL_LogOutputFunction forward = nullptr;
	void* forwardUserdata = nullptr;
};

/*
 * 为 ImGui 加载包含中文字形的字体。
 *
 * ImGui 自带的默认字体只覆盖基础拉丁字符。UTF-8 中文字符串虽然没有损坏，
 * 但渲染时找不到对应字形，就会显示成问号。这里先检查用户通过环境变量
 * VISIBLEAUDIO_FONT 指定的字体，再尝试各平台常见的中文系统字体。
 *
 * 如果以后希望发布时完全不依赖系统字体，可把 Noto Sans CJK 放到
 * assets/fonts/，下面的第一个固定候选路径会自动找到它。
 */
static bool LoadChineseFont(ImGuiIO& io)
{
	std::vector<std::string> candidates;
	if (const char* customFont = SDL_getenv("VISIBLEAUDIO_FONT"))
		candidates.emplace_back(customFont);

	// 第一个是可选的随程序分发字体；其余为 Windows、Linux、macOS 常见路径。
	const char* knownFonts[] = {
		"assets/fonts/NotoSansCJKsc-Regular.otf",
		"C:/Windows/Fonts/msyh.ttc",
		"C:/Windows/Fonts/simhei.ttf",
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
		"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		"/System/Library/Fonts/PingFang.ttc",
		"/System/Library/Fonts/STHeiti Light.ttc",
		"/Library/Fonts/Arial Unicode.ttf"
	};
	candidates.insert(candidates.end(), std::begin(knownFonts), std::end(knownFonts));

	for (const std::string& path : candidates) {
		std::error_code error;
		if (!std::filesystem::is_regular_file(path, error)) continue;

		// 当前 ImGui 支持按需加载字形，不需要把两万多个 CJK 字形预烘焙进纹理。
		if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f)) {
			// 显式指定默认字体，避免以后先添加图标字体等其他字体时改变选择顺序。
			io.FontDefault = font;
			// 这条中文日志既是状态提示，也是最直观的 UTF-8/中文字形自检。
			SDL_Log("中文字体加载成功：%s", path.c_str());
			return true;
		}
	}

	SDL_Log("no CJK font found; set VISIBLEAUDIO_FONT to a Chinese font file");
	return false;
}

// 将 SDL 的枚举值转换成适合显示的短字符串。
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

/*
 * SDL 日志回调。
 * SDLCALL 是 SDL 定义的调用约定；签名必须与 SDL_LogOutputFunction 完全一致。
 * userdata 是注册回调时传入的 AppLog 地址，这种“函数指针 + void* 上下文”
 * 是 C 风格库里很常见的回调写法。
 */
static void SDLCALL CaptureLog(void* userdata, int category,
	SDL_LogPriority priority, const char* message)
{
	auto* log = static_cast<AppLog*>(userdata);
	std::string line = "[";
	line += LogPriorityName(priority);
	line += "] ";
	line += message ? message : "";

	{
		// lock_guard 使用 RAII：离开这对花括号时会自动解锁，即使发生异常也一样。
		std::lock_guard<std::mutex> lock(log->mu);
		log->lines.push_back(std::move(line));
		// 日志有上限，避免程序运行很久后持续占用内存。
		while (log->lines.size() > 200) log->lines.pop_front();
	}

	// 除了保存 UI 副本，也继续输出到 SDL 原来的控制台/调试器目标。
	if (log->forward) log->forward(log->forwardUserdata, category, priority, message);
}

// 把日志画在频谱柱后面。先画日志、后画柱子就自然形成“背景日志”效果。
static void DrawLogBackground(ImDrawList* drawList, ImVec2 min, ImVec2 size,
	AppLog& log)
{
	// 持锁时只复制数据，不做绘图，尽量缩短其他日志线程的等待时间。
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> lock(log.mu);
		lines.assign(log.lines.begin(), log.lines.end());
	}

	const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
	const int visibleLines = std::max(0, (int)((size.y - 36.0f) / lineHeight));
	const int first = std::max(0, (int)lines.size() - visibleLines);
	ImVec2 max(min.x + size.x, min.y + size.y);
	// 裁剪矩形保证过长日志不会画到 Spectrum 面板以外。
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

/*
 * WAV 播放设备的 post-mix 回调。
 * SDL 在真正把声音交给硬件之前调用它，因此这里看到的是最终浮点混音结果。
 * buflen 的单位是“字节”而不是“采样数”；一帧包含 spec->channels 个 float。
 *
 * 注意：这是实时音频线程。这里不能操作 ImGui，也不应该做 FFT 等耗时工作。
 */
static void SDLCALL PostMix(void* userdata, const SDL_AudioSpec* spec,
	float* buffer, int buflen)
{
	auto* v = static_cast<Vis*>(userdata);
	int frames = buflen / (int)(spec->channels * sizeof(float));
	std::lock_guard<std::mutex> lock(v->mu);
	v->in.reserve(v->in.size() + frames);
	for (int i = 0; i < frames; i++) {
		// 将一帧里的所有声道取平均，得到单声道采样。
		float s = 0.0f;
		for (int c = 0; c < spec->channels; c++) s += buffer[i * spec->channels + c];
		v->in.push_back(s / (float)spec->channels);
	}
	if ((int)v->in.size() > MAX_PENDING_SAMPLES)
		v->in.erase(v->in.begin(), v->in.end() - MAX_PENDING_SAMPLES);
}

// RtAudio 实时输出采集的回调入口。RtAudio 层已经把数据转换成单声道 float。
static void ReceiveRealtimeSamples(void* userdata, const float* samples,
	std::size_t sampleCount)
{
	auto* v = static_cast<Vis*>(userdata);
	std::lock_guard<std::mutex> lock(v->mu);
	v->in.insert(v->in.end(), samples, samples + sampleCount);
	if ((int)v->in.size() > MAX_PENDING_SAMPLES)
		v->in.erase(v->in.begin(), v->in.end() - MAX_PENDING_SAMPLES);
}

// 切换音频源时清除旧源留下的数据和柱高，避免画面短暂显示上一首/上一源。
static void ClearVisualization(Vis& v)
{
	std::lock_guard<std::mutex> lock(v.mu);
	v.in.clear();
	std::fill(std::begin(v.mag), std::end(v.mag), 0.0f);
	std::fill(std::begin(v.peak), std::end(v.peak), 0.0f);
}

/*
 * 对最近 FFT_N 个采样执行实数 FFT，并更新 BAR_COUNT 根柱子。
 *
 * FFT 输出 FFT_N / 2 + 1 个有效频点：实数输入的负频率部分与正频率对称，
 * 所以无需重复保存。随后把大量频点分组成 80 段，再把每段能量映射到 [0, 1]。
 */
static void ComputeFFT(Vis& v)
{
	std::lock_guard<std::mutex> lock(v.mu);
	// 数据不足一个 FFT 窗口时先等待，避免越界读取。
	if ((int)v.in.size() < FFT_N) return;

	// static 局部变量只初始化一次，可避免每帧重复创建 FFT 配置和工作缓冲。
	static kiss_fftr_cfg cfg = kiss_fftr_alloc(FFT_N, 0, nullptr, nullptr);
	static std::vector<kiss_fft_scalar> tmp(FFT_N);
	static std::vector<float> window(FFT_N);
	static bool once = false;
	if (!once) {
		// Hann 窗降低“频谱泄漏”：有限长度截断产生的边缘突变会被平滑掉。
		for (int i = 0; i < FFT_N; i++)
			window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_N - 1)));
		once = true;
	}
	// 总是分析最新采样，避免 UI 帧率低时画面落后于实际声音。
	auto newest = v.in.end() - FFT_N;
	for (int i = 0; i < FFT_N; i++) tmp[i] = newest[i] * window[i];
	v.in.erase(v.in.begin(), v.in.end() - FFT_N / 2);

	// 每个复数频点包含实部 r 和虚部 i，平方和代表该频点的能量。
	std::vector<kiss_fft_cpx> out(FFT_N / 2 + 1);
	kiss_fftr(cfg, tmp.data(), out.data());

	for (int b = 0; b < BAR_COUNT; b++) {
		// 二次曲线让低频区域分得更细，更符合音乐可视化的观感。
		float lo = powf((b + 0.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
		float hi = powf((b + 1.0f) / BAR_COUNT, 2.0f) * (FFT_N / 2);
		int i0 = std::clamp((int)floorf(lo), 1, FFT_N / 2);
		int i1 = std::clamp((int)ceilf(hi), i0 + 1, FFT_N / 2 + 1);
		float sum = 0.0f;
		for (int i = i0; i < i1; i++) sum += out[i].r * out[i].r + out[i].i * out[i].i;
		// kissfft 不会归一化输出；先归一化再转 dB，否则非静音柱几乎都会顶格。
		float magnitude = sqrtf(sum / (float)(i1 - i0)) * (4.0f / FFT_N);
		// 1e-9 防止静音时 log10(0) 得到负无穷。
		float db = 20.0f * log10f(magnitude + 1e-9f);
		float val = std::clamp((db + 70.0f) / 70.0f, 0.0f, 1.0f);
		// 0.35 是柱高的平滑系数；峰值下降得更慢，形成白色峰值线。
		v.mag[b] += (val - v.mag[b]) * 0.35f;
		v.peak[b] = (val > v.peak[b]) ? val : v.peak[b] * 0.97f;
	}
}

// 根据用户选择的基础色和柱高生成柱子的实际颜色。只调整 HSV 的亮度，
// 因此用户选定的色相/饱和度在不同音量下仍保持一致。
static ImU32 BarColorForMagnitude(const ImVec4& baseColor, float magnitude,
	float hueOffset)
{
	float h = 0.0f, s = 0.0f, value = 0.0f;
	ImGui::ColorConvertRGBtoHSV(baseColor.x, baseColor.y, baseColor.z, h, s, value);
	h = fmodf(h + hueOffset, 1.0f);
	if (h < 0.0f) h += 1.0f;
	const float brightness = 0.20f + std::clamp(magnitude, 0.0f, 1.0f) * 0.80f;
	float r = 0.0f, g = 0.0f, b = 0.0f;
	ImGui::ColorConvertHSVtoRGB(h, s, value * brightness, r, g, b);
	return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, baseColor.w));
}

// 读取整个 WAV、创建 SDL 播放流，并把 post-mix 回调接到同一个 Vis 缓冲。
static bool LoadAndPlay(const char* path, SDL_AudioDeviceID& dev,
	SDL_AudioStream*& stream, Vis& vis)
{
	if (stream) {
		// SDL_OpenAudioDeviceStream 创建的 stream 拥有其逻辑设备，销毁流即可关闭设备。
		SDL_DestroyAudioStream(stream);
		stream = nullptr;
		dev = 0;
	}
	// SDL_LoadWAV 会填充格式 spec，并用 SDL 分配器返回完整的音频数据。
	SDL_AudioSpec spec;
	Uint8* data = nullptr; Uint32 len = 0;
	if (!SDL_LoadWAV(path, &spec, &data, &len)) {
		SDL_Log("load wav fail: %s", SDL_GetError());
		return false;
	}
	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&spec, nullptr, nullptr);
	if (!stream) { SDL_Log("open stream fail: %s", SDL_GetError()); SDL_free(data); return false; }

	// post-mix 回调挂在“设备”而不是 stream 上，所以要从 stream 取得真实设备 ID。
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

	// Put 会把数据复制/排队，因此调用后即可 SDL_free 原始 WAV 缓冲。
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
	// 尽早接管 SDL 日志，初始化阶段的错误也能进入界面日志队列。
	AppLog appLog;
	SDL_GetLogOutputFunction(&appLog.forward, &appLog.forwardUserdata);
	SDL_SetLogOutputFunction(CaptureLog, &appLog);
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
		SDL_Log("SDL initialization failed: %s", SDL_GetError());
		SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
		return 1;
	}

	// SDL 负责原生窗口、事件、音频播放和最终的 2D 绘制。根据当前显示器的
	// 可用区域选择初始尺寸，避免在低分辨率/缩放显示器上创建一个超出屏幕的窗口。
	int initialWidth = 1280;
	int initialHeight = 720;
	SDL_Rect displayBounds{};
	if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &displayBounds)) {
		initialWidth = std::clamp(displayBounds.w - 40, 640, initialWidth);
		initialHeight = std::clamp(displayBounds.h - 80, 360, initialHeight);
	}
	SDL_Window* win = SDL_CreateWindow("Audio Visualizer", initialWidth, initialHeight,
		// 使用逻辑分辨率作为渲染目标，避免 Retina/高 DPI 下 drawable
		// 自动变成窗口尺寸的 2 倍，导致布局和实际绘制区域不一致。
		SDL_WINDOW_RESIZABLE);
	if (!win) {
		SDL_Log("window creation failed: %s", SDL_GetError());
		SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
		SDL_Quit();
		return 1;
	}
	SDL_SetWindowMinimumSize(win, 480, 320);
	SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
	if (!ren) {
		SDL_Log("renderer creation failed: %s", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
		SDL_Quit();
		return 1;
	}

	// ImGui 本身只生成界面顶点；下面两个 backend 分别连接 SDL 事件和 SDL Renderer。
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	LoadChineseFont(io);
	// 默认字号对高分辨率窗口显得过小。字体和控件一起放大，保证文字、按钮
	// 与频谱面板在 1280x720 及更大窗口中都具有可读尺寸。
	ImGui::GetStyle().ScaleAllSizes(1.0f);
	ImGui::GetStyle().FontScaleMain = 1.0f;
	ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
	ImGui_ImplSDLRenderer3_Init(ren);
	ImGui::StyleColorsDark();
	const ImGuiStyle baseStyle = ImGui::GetStyle();

	// WAV 模式由 SDL stream 播放；实时模式由 LoopbackCapture 采集。
	// 任一时刻界面只启用其中一个来源。
	SDL_AudioDeviceID dev = 0;      // 0 表示当前没有 WAV 播放设备。
	SDL_AudioStream* stream = nullptr; // SDL 对象用指针表示；nullptr 表示不存在。
	Vis vis;
	LoopbackCapture loopback;
	char pathBuf[512] = "assets/test.wav";
	int sourceMode = 0; // ImGui Combo 使用整数索引：0=WAV，1=实时系统输出。
	bool limitFps = true;
	int targetFps = 60;
	ImVec4 barColor(0.20f, 0.65f, 1.00f, 1.00f);
	bool animateThemeColor = false;
	bool movingBarGradient = false;
	int lastLoggedWindowW = 0, lastLoggedWindowH = 0;
	int lastLoggedDrawableW = 0, lastLoggedDrawableH = 0;
	SDL_Log("VisibleAudio started");

	bool run = true;
	while (run) {
		// 每轮 while 就是一帧。记录起点，帧末才能计算还需要休眠多久。
		const Uint64 frameStart = SDL_GetTicksNS();

		// 先清空 SDL 事件队列。必须把事件交给 ImGui，它才能响应鼠标和键盘。
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT) run = false;
		}
		// FFT 放在主线程执行，避免耗时计算阻塞实时音频回调。
		ComputeFFT(vis);

		// 告诉两个 ImGui backend 和 ImGui 核心：开始构建新的一帧界面。
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		// 主视口的 Pos/Size 是当前窗口完整客户区；窗口缩放后布局会自动跟随。
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		int windowW = 0, windowH = 0;
		int drawableW = 0, drawableH = 0;
		SDL_GetWindowSize(win, &windowW, &windowH);
		SDL_GetWindowSizeInPixels(win, &drawableW, &drawableH);
		// ImGui 的 viewport 在部分高 DPI 后端返回的是绘制像素尺寸，而窗口
		// 布局使用逻辑尺寸。SDL_GetWindowSize 始终返回当前客户区逻辑尺寸，
		// 用它计算面板可避免内容只占窗口一部分或超出窗口边界。
		const ImVec2 workPos(0.0f, 0.0f);
		const ImVec2 workSize((float)std::max(1, windowW), (float)std::max(1, windowH));
		const bool layoutSizeChanged = windowW != lastLoggedWindowW || windowH != lastLoggedWindowH ||
			drawableW != lastLoggedDrawableW || drawableH != lastLoggedDrawableH;
		if (layoutSizeChanged) {
			SDL_Log("layout sizes: window=%dx%d drawable=%dx%d imgui_display=%.0fx%.0f viewport=%.0f,%.0f %.0fx%.0f",
				windowW, windowH, drawableW, drawableH, io.DisplaySize.x, io.DisplaySize.y,
				viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);
			lastLoggedWindowW = windowW;
			lastLoggedWindowH = windowH;
			lastLoggedDrawableW = drawableW;
			lastLoggedDrawableH = drawableH;
		}
		// 随窗口高度调整字体和控件比例，避免 4K 窗口文字过小、低分辨率窗口
		// 又因控件过大而被截断。样式以启动时的基准副本重新计算，避免逐帧累乘。
		const float resolutionScale = std::clamp(workSize.y / 720.0f, 0.80f, 1.50f);
		ImGui::GetStyle() = baseStyle;
		ImGui::GetStyle().ScaleAllSizes(resolutionScale);
		ImGui::GetStyle().FontScaleMain = 1.10f * resolutionScale;
		// 窄窗口改为竖排，给输入框和按钮保留足够宽度；宽窗口继续使用横排。
		// 所有尺寸都来自当前客户区，因此拖动窗口或改变 DPI 后下一帧即可重排。
		const bool compactLayout = workSize.x < 760.0f;
		const float fpsPanelWidth = compactLayout ? workSize.x
			: std::min(300.0f, workSize.x * 0.32f);
		const float controlWidth = compactLayout ? workSize.x
			: std::max(1.0f, workSize.x - fpsPanelWidth);
		const float controlHeight = compactLayout
			? std::min(210.0f, std::max(170.0f, workSize.y * 0.26f))
			: std::min(180.0f, std::max(150.0f, workSize.y * 0.30f));
		const float fpsHeight = compactLayout
			? std::min(120.0f, std::max(96.0f, workSize.y * 0.16f))
			: controlHeight;
		// 三个面板由程序固定布局，不允许用户拖动或把错误尺寸保存进 imgui.ini。
		constexpr ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoSavedSettings;

		// 左上：音频源控制面板。明确设置位置/尺寸也修复了旧配置把频谱保存成
		// 32x35 像素后无法画柱子的问题。
		ImGui::SetNextWindowPos(workPos);
		ImGui::SetNextWindowSize(ImVec2(controlWidth, controlHeight));
		ImGui::Begin("Control", nullptr, panelFlags);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::Combo("##Audio source", &sourceMode,
			"WAV file\0System output (live)\0")) {
			// 切换来源前先停止两种旧来源。销毁 SDL stream 会等待其回调安全结束。
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
			// WAV 模式：路径是相对于程序“工作目录”的，不一定相对于 exe 所在目录。
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
		}
		else {
			// 实时模式：Start() 会自动寻找默认输出/monitor/虚拟回采设备。
			if (!loopback.IsRunning()) {
				if (ImGui::Button("Start capture")) {
					ClearVisualization(vis);
					std::string error;
					if (!loopback.Start(ReceiveRealtimeSamples, &vis, error))
						SDL_Log("start real-time output capture failed: %s", error.c_str());
				}
			}
			else if (ImGui::Button("Stop capture")) {
				loopback.Stop();
				SDL_Log("real-time output capture stopped");
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(loopback.IsRunning() ? "Capturing" : "Stopped");
		}
		auto drawAnimateThemeCheckbox = [&]() {
			if (ImGui::Checkbox("Animate theme color", &animateThemeColor)) {
				if (animateThemeColor) movingBarGradient = false;
				SDL_Log("animated theme color %s", animateThemeColor ? "enabled" : "disabled");
			}
		};
		auto drawMovingGradientCheckbox = [&]() {
			if (ImGui::Checkbox("Moving bar color gradient", &movingBarGradient)) {
				if (movingBarGradient) animateThemeColor = false;
				SDL_Log("moving bar color gradient %s", movingBarGradient ? "enabled" : "disabled");
			}
		};
		if (compactLayout) {
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::ColorEdit3("Bar color", &barColor.x,
				ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoInputs))
				SDL_Log("bar color changed to %.2f, %.2f, %.2f", barColor.x, barColor.y, barColor.z);
			drawAnimateThemeCheckbox();
			drawMovingGradientCheckbox();
		}
		else {
			// 宽窗口时把颜色选择器和两个选项放在同一行，选项位于右侧。
			const float effectWidth = 300.0f;
			ImGui::SetNextItemWidth(std::max(120.0f, controlWidth - effectWidth));
			if (ImGui::ColorEdit3("Bar color", &barColor.x,
				ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoInputs))
				SDL_Log("bar color changed to %.2f, %.2f, %.2f", barColor.x, barColor.y, barColor.z);
			ImGui::SameLine();
			drawAnimateThemeCheckbox();
			ImGui::SameLine();
			drawMovingGradientCheckbox();
		}
		if (layoutSizeChanged)
			SDL_Log("panel Control actual: pos=%.0f,%.0f size=%.0fx%.0f",
				ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
				ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
		ImGui::End();

		// FPS 限制面板：宽窗口位于右上，窄窗口自动移到控制面板下方。
		const float fpsY = compactLayout ? workPos.y + controlHeight : workPos.y;
		ImGui::SetNextWindowPos(ImVec2(compactLayout ? workPos.x : workPos.x + controlWidth, fpsY));
		ImGui::SetNextWindowSize(ImVec2(fpsPanelWidth, fpsHeight));
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
		if (layoutSizeChanged)
			SDL_Log("panel FPS actual: pos=%.0f,%.0f size=%.0fx%.0f",
				ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
				ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
		ImGui::End();

		// 频谱画布占用剩余全部空间。
		const float spectrumY = compactLayout ? fpsY + fpsHeight : workPos.y + controlHeight;
		if (layoutSizeChanged) {
			SDL_Log("layout panels: compact=%s control=%.0fx%.0f fps=%.0fx%.0f spectrum_y=%.0f spectrum_h=%.0f scale=%.2f",
				compactLayout ? "yes" : "no", controlWidth, controlHeight,
				fpsPanelWidth, fpsHeight, spectrumY,
				workPos.y + workSize.y - spectrumY, resolutionScale);
		}
		ImGui::SetNextWindowPos(ImVec2(workPos.x, spectrumY));
		ImGui::SetNextWindowSize(ImVec2(workSize.x,
			std::max(1.0f, workPos.y + workSize.y - spectrumY)));
		ImGui::Begin("Spectrum", nullptr, panelFlags);
		if (layoutSizeChanged)
			SDL_Log("panel Spectrum actual: pos=%.0f,%.0f size=%.0fx%.0f",
				ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
				ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
		ImVec2 cv = ImGui::GetContentRegionAvail();
		ImDrawList* d = ImGui::GetWindowDrawList();
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		// w 是每根柱可用的水平宽度，h 是画布高度；两者单位都是像素。
		float w = cv.x / BAR_COUNT, h = cv.y;
		float pad = std::min(1.0f, w * 0.15f);
		const float animationTime = (float)(SDL_GetTicksNS() / 1000000000.0);
		const float animationSpeed = 0.08f;
		DrawLogBackground(d, p0, cv, appLog);
		for (int i = 0; i < BAR_COUNT; i++) {
			// 柱高越大，基础色的 HSV 亮度越高。
			float hueOffset = 0.0f;
			if (animateThemeColor)
				hueOffset = animationTime * animationSpeed;
			else if (movingBarGradient)
				hueOffset = animationTime * animationSpeed + (float)i / BAR_COUNT;
			ImU32 col = BarColorForMagnitude(barColor, vis.mag[i], hueOffset);
			float bh = vis.mag[i] * h * 0.9f;
			d->AddRectFilled(ImVec2(p0.x + i * w + pad, p0.y + h - bh),
				ImVec2(p0.x + (i + 1) * w - pad, p0.y + h), col, 2.0f);
			float ph = vis.peak[i] * h * 0.9f;
			d->AddRectFilled(ImVec2(p0.x + i * w + pad, p0.y + h - ph - 3),
				ImVec2(p0.x + (i + 1) * w - pad, p0.y + h - ph), 0xFFFFFFFF, 1.0f);
		}
		// DrawList 绘图不会自动参与 ImGui 布局，用 Dummy 声明画布占用的空间。
		ImGui::Dummy(cv);
		ImGui::End();

		// ImGui::Render 只整理绘图命令；RenderDrawData 才把它们交给 SDL Renderer。
		ImGui::Render();
		SDL_SetRenderDrawColor(ren, 18, 18, 26, 255);
		SDL_RenderClear(ren);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
		SDL_RenderPresent(ren);

		if (limitFps) {
			// 目标帧时间 = 1 秒 / FPS。只休眠当前帧尚未用完的部分。
			const Uint64 targetFrameTime = 1000000000ULL / (Uint64)targetFps;
			const Uint64 elapsed = SDL_GetTicksNS() - frameStart;
			if (elapsed < targetFrameTime) SDL_DelayPrecise(targetFrameTime - elapsed);
		}
	}

	// 按依赖关系逆序释放资源：先停止会访问 Vis 的后台采集，再销毁 UI 和 SDL。
	loopback.Stop();
	ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	if (stream) SDL_DestroyAudioStream(stream);
	SDL_Log("VisibleAudio stopped");
	SDL_SetLogOutputFunction(appLog.forward, appLog.forwardUserdata);
	SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
	return 0;
}
