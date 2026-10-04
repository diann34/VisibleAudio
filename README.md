# VisibleAudio
注：纯Vibe Coding项目

VisibleAudio 是一个适合学习 C++ 音频编程的小型频谱可视化项目。它可以播放 WAV 文件，也可以实时采集系统正在播放的声音，然后用 FFT（快速傅里叶变换）把声音画成 80 根频谱柱。

项目主要使用：

- SDL3：窗口、事件、WAV 播放和 2D 渲染。
- Dear ImGui：按钮、下拉框、FPS 面板和自定义画布。
- kissfft：把时域采样转换成频域数据。
- RtAudio：用统一接口采集不同操作系统的实时音频输出。
- vcpkg：下载、编译并向 CMake 提供第三方依赖。

## 功能

- 播放 WAV 文件并显示实时频谱。
- 在 WAV 文件和系统实时输出之间切换。
- 80 根平滑频谱柱和缓慢下降的峰值线。
- 可开关的 15–240 FPS 限制。
- 在频谱背景中显示最近 200 条程序日志。
- 自动加载常见中文字体，避免中文日志显示成问号。
- Windows 下默认静态链接 SDL3、kissfft、RtAudio 和 MSVC 运行库。

## 项目结构

```text
VisibleAudio/
├─ assets/
│  └─ test.wav                 默认测试音频
├─ extern/
│  ├─ imgui/                   Dear ImGui 子模块
│  └─ vcpkg/                   vcpkg 子模块
├─ src/
│  ├─ main.cpp                 主循环、WAV 播放、FFT、界面和绘图
│  ├─ loopback_capture.h       实时输出采集的公开接口
│  └─ loopback_capture.cpp     基于 RtAudio 的跨平台实现
├─ CMakeLists.txt              定义如何编译和链接项目
├─ CMakePresets.json           Debug/Release 构建预设
└─ vcpkg.json                  第三方依赖清单
```

建议按下面的顺序阅读源码：

1. 先读 `loopback_capture.h`，了解实时采集类对外提供什么功能。
2. 再读 `main.cpp` 顶部的 `Vis`、`PostMix()` 和 `ReceiveRealtimeSamples()`。
3. 接着读 `ComputeFFT()`，理解采样如何变成柱高。
4. 最后读 `main()`，观察事件、界面、绘图和 FPS 限制如何组成每一帧。
5. 对平台设备选择感兴趣时，再读 `loopback_capture.cpp`。

## 数据是怎样流动的

两个音频源最终都会汇入同一个 `Vis::in` 单声道采样缓冲区：

```text
WAV 文件 ── SDL 播放 ── PostMix() ──────────────┐
                                                ├─> Vis::in
系统输出 ── RtAudio ── ReceiveRealtimeSamples() ┘
                                                     │
                                                     ▼
                                               ComputeFFT()
                                                     │
                                                     ▼
                                           mag[80] + peak[80]
                                                     │
                                                     ▼
                                             ImGui DrawList
                                                     │
                                                     ▼
                                              SDL Renderer
```

这样设计有一个好处：WAV 和实时输出只有“取得采样”的方式不同，后面的 FFT 与绘图代码完全共用。

## 三个重要线程

程序中并不是所有代码都在同一条线程上运行。

| 线程 | 执行的代码 | 主要职责 |
|---|---|---|
| 主线程 | `main()`、`ComputeFFT()`、ImGui | 处理事件、计算频谱、构建并绘制界面 |
| SDL 音频线程 | `PostMix()` | 取得 WAV 最终播放数据并写入采样缓冲 |
| RtAudio 线程 | `AudioCallback()` | 取得系统输出，混成单声道后写入采样缓冲 |

音频线程和主线程都会访问 `Vis::in`。如果一个线程正在扩展 `std::vector`，另一个线程同时读取它，程序就产生“数据竞争”，结果是未定义行为。因此 `Vis` 中包含 `std::mutex mu`，相关代码使用：

```cpp
std::lock_guard<std::mutex> lock(v.mu);
```

`lock_guard` 创建时加锁，离开当前花括号时自动解锁。这是 C++ 常见的 RAII 用法。

## FFT 入门

麦克风或音频文件给出的原始数据是一串随时间变化的振幅，称为“时域数据”。FFT 把它转换成各个频率有多强，称为“频域数据”。

本项目一次使用 2048 个采样：

```cpp
static constexpr int FFT_N = 2048;
```

假设设备采样率为 48,000 Hz，这一小段声音的时长约为：

```text
2048 / 48000 ≈ 0.0427 秒
```

主要步骤如下：

1. 等待至少积累 2048 个采样。
2. 取最新的 2048 个采样，避免画面落后于实时声音。
3. 乘以 Hann 窗，减轻截断边缘造成的频谱泄漏。
4. 调用 `kiss_fftr()` 得到复数频点。
5. 用 `实部² + 虚部²` 计算能量。
6. 把大量频点分组为 80 个频段。
7. 转换成 dB，再映射到 `[0, 1]` 作为柱高。
8. 对柱高做平滑，并让峰值线缓慢下降。

代码里跳过了第 0 个频点，因为它是 DC（直流）分量，通常不属于我们想看的声音频率。

## C++ 语法提示

### 指针与 `nullptr`

SDL 的许多资源通过指针表示：

```cpp
SDL_AudioStream* stream = nullptr;
```

`nullptr` 表示现在没有对象。创建成功后它指向一个 SDL 对象；使用结束后调用对应的 `SDL_Destroy...()`，并重新设为 `nullptr`。

### 引用参数

```cpp
static void ClearVisualization(Vis& v)
```

`Vis&` 是引用。函数操作的是调用者原来的 `Vis`，不会复制一份昂贵的新对象。

### 回调函数

SDL 和 RtAudio 都在需要数据时“反过来调用”我们的函数。因为 C 风格回调不知道 C++ 对象，所以接口通常同时接收：

- 一个函数指针；
- 一个 `void* userdata`，用来带回调用者自己的对象地址。

回调里再使用 `static_cast<Vis*>(userdata)` 恢复原类型。

### RAII

`std::lock_guard`、`std::unique_ptr` 和 `LoopbackCapture` 析构函数都体现了 RAII：资源由对象拥有，对象离开作用域时资源自动释放。它能显著减少忘记解锁、忘记关闭设备和内存泄漏。

### PImpl

`loopback_capture.h` 只声明了：

```cpp
struct Impl;
std::unique_ptr<Impl> impl_;
```

而 `Impl` 的完整内容放在 `.cpp` 文件。这叫 PImpl。它让主程序不需要包含 `RtAudio.h`，也把第三方库细节限制在一个文件中。

## 构建环境

最低建议：

- CMake 3.20 或更新版本；
- 支持 C++20 的编译器；
- Ninja；
- Git；
- Windows：Visual Studio C++ 工作负载和 Windows SDK；
- Linux：常规 C++ 工具链，以及 PulseAudio 开发环境（vcpkg 会处理库依赖）；
- macOS：Xcode Command Line Tools。

克隆项目时需要同时取得子模块：

```bash
git clone --recursive <项目地址>
```

如果已经克隆但 `extern/imgui` 或 `extern/vcpkg` 是空目录：

```bash
git submodule update --init --recursive
```

首次使用 vcpkg 时需要引导它：

Windows PowerShell：

```powershell
./extern/vcpkg/bootstrap-vcpkg.bat
```

Linux/macOS：

```bash
./extern/vcpkg/bootstrap-vcpkg.sh
```

## 配置与编译

### Windows

建议从“Developer PowerShell for Visual Studio”运行，确保编译器和 Windows SDK 路径完整：

```powershell
cmake --preset dev
cmake --build --preset dev
```

Release 构建：

```powershell
cmake --preset release
cmake --build --preset release
```

输出位置：

```text
build/bin/Debug/VisibleAudio.exe
build/bin/Release/VisibleAudio.exe
```

Windows 默认使用 `x64-windows-static` triplet，因此第三方库和 MSVC 运行库会静态链接到 EXE。首次构建 SDL3 可能需要几分钟，之后通常会命中 vcpkg 缓存。

### Linux/macOS

预设不会强制 Windows triplet，vcpkg 会选择当前平台的默认 triplet：

```bash
cmake --preset dev
cmake --build --preset dev
```

Linux 构建会给 RtAudio 启用 PulseAudio 后端，以便发现 PulseAudio/PipeWire monitor。

## 运行

建议从项目根目录启动，这样默认相对路径 `assets/test.wav` 才能正确解析：

```powershell
./build/bin/Release/VisibleAudio.exe
```

界面包含三个区域：

- `Control`：选择 WAV 或系统实时输出，并控制播放/采集。
- `FPS Limit`：启用限制并选择目标 FPS。
- `Spectrum`：显示背景日志、频谱柱和白色峰值线。

### WAV 模式

1. 在音频源下拉框选择 `WAV file`。
2. 输入 WAV 路径。
3. 点击 `Play`。
4. 使用 `Pause` 和 `Resume` 控制播放。

### 实时系统输出模式

1. 选择 `System output (live)`。
2. 点击 `Start capture`。
3. 在其他播放器中播放声音。
4. 点击 `Stop capture` 停止。

不同系统的回采方式：

- Windows：RtAudio 使用默认输出设备的 WASAPI loopback。
- Linux：优先寻找名称包含 `monitor` 的 PulseAudio/PipeWire 输入源。
- macOS：系统通常不直接暴露总输出，需要安装 BlackHole 或 Soundflower 等虚拟回采设备。

## 中文日志字体

ImGui 默认字体没有中文字形。本项目会按顺序尝试：

1. 环境变量 `VISIBLEAUDIO_FONT` 指定的字体；
2. `assets/fonts/NotoSansCJKsc-Regular.otf`；
3. Windows 的微软雅黑/黑体；
4. Linux 的 Noto Sans CJK/文泉驿；
5. macOS 的苹方/华文黑体。

如果仍显示问号，可以手动指定字体。

Windows PowerShell：

```powershell
$env:VISIBLEAUDIO_FONT = "C:/Windows/Fonts/msyh.ttc"
./build/bin/Release/VisibleAudio.exe
```

Linux/macOS：

```bash
VISIBLEAUDIO_FONT=/path/to/NotoSansCJK-Regular.ttc ./build/bin/Release/VisibleAudio
```

字体内容必须包含中文字形，仅把普通英文字体改名并不能解决问题。

## 常见问题

### WAV 能播放，但没有频谱柱

- 确认运行的是最新构建的程序。
- 查看频谱背景日志里是否有加载或音频设备错误。
- 确认 Spectrum 面板有足够高度。
- 确认 WAV 本身不是静音文件。

### 找不到 `assets/test.wav`

相对路径以“当前工作目录”为基准。请从项目根目录启动，或者在界面里输入绝对路径。

### 实时输出显示 `No usable system-output loopback device`

- Windows：确认默认输出设备有效，且其他程序确实能播放声音。
- Linux：确认使用 PulseAudio 或 PipeWire 的 PulseAudio 兼容层，并存在 monitor source。
- macOS：安装并配置 BlackHole/Soundflower，然后重新启动程序。

### 编译器找不到 `stdarg.h` 或 `kernel32.lib`

这通常不是源码错误，而是 Windows 普通终端没有加载 Visual Studio 开发环境。请使用 Developer PowerShell，或者先调用 `vcvars64.bat`。

### 链接器提示无法写入 `VisibleAudio.exe`

旧程序仍在运行时，Windows 会锁住 EXE。关闭正在运行的 VisibleAudio 后重新构建。

### 首次构建很慢

vcpkg 需要从源码编译静态 SDL3、RtAudio 和 kissfft。完成一次后会使用二进制缓存，后续构建会快很多。

## 适合继续练习的方向

- 在控制面板加入柱子数量和频谱平滑系数调节。
- 根据真实采样率把横轴标成 Hz。
- 为低频、中频、高频使用不同颜色。
- 把 WAV 一次性加载改成分块流式读取。
- 给 `ComputeFFT()` 编写纯数据单元测试。
- 增加实时设备列表，让用户手动选择 monitor/虚拟设备。
