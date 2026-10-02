cmake_minimum_required(VERSION 3.20)

# 这段在 project() 之前执行，因为 vcpkg toolchain 会在 project() 期间读取 triplet。
# vcpkg's Windows default is dynamic. Prefer a self-contained static build,
# while leaving Linux/macOS free to use their native default triplets. Users
# can still override VCPKG_TARGET_TRIPLET for cross-compilation or another CPU.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING
        "vcpkg target triplet")
endif()

project(VisibleAudio LANGUAGES CXX)

# 所有 target 使用 C++20。多配置生成器会把不同配置放在 bin/Debug、bin/Release。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
# x64-windows-static 的依赖使用 /MT；项目也必须一致，否则链接器会报告 CRT 冲突。
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- 由 vcpkg.json 安装，并由 vcpkg toolchain 暴露给 CMake 的库 ----
find_package(SDL3 CONFIG REQUIRED)
find_package(kissfft CONFIG REQUIRED)
find_package(RtAudio CONFIG REQUIRED)

# ---- ImGui 是源码型子模块：核心文件 + SDL3 事件/渲染后端一起编译 ----
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/extern/imgui)
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# PUBLIC 表示链接 imgui_lib 的 VisibleAudio 也会继承 SDL3 和头文件搜索路径。
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3)
target_compile_definitions(imgui_lib PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# ---- 主程序：WIN32 让 Windows Release 不额外弹出控制台窗口 ----
add_executable(VisibleAudio WIN32
    src/main.cpp
    src/loopback_capture.cpp
)
target_link_libraries(VisibleAudio PRIVATE
    SDL3::SDL3
    imgui_lib
    kissfft::kissfft-float
    RtAudio::rtaudio
)
# MSVC 与 GCC/Clang 的警告选项写法不同，因此分别设置。
if(MSVC)
    target_compile_options(VisibleAudio PRIVATE /W4 /utf-8)
else()
    target_compile_options(VisibleAudio PRIVATE -Wall -Wextra -Wpedantic)
endif()
