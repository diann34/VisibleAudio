cmake_minimum_required(VERSION 3.20)

# vcpkg's Windows default is dynamic. Prefer a self-contained static build,
# while leaving Linux/macOS free to use their native default triplets. Users
# can still override VCPKG_TARGET_TRIPLET for cross-compilation or another CPU.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING
        "vcpkg target triplet")
endif()

project(VisibleAudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- 由 vcpkg 提供的库 ----
find_package(SDL3 CONFIG REQUIRED)
find_package(kissfft CONFIG REQUIRED)
find_package(RtAudio CONFIG REQUIRED)

# ---- ImGui 由 submodule 编译（核心 + SDL3 后端）----
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
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3)
target_compile_definitions(imgui_lib PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# ---- 主程序 ----
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
if(MSVC)
    target_compile_options(VisibleAudio PRIVATE /W4 /utf-8)
else()
    target_compile_options(VisibleAudio PRIVATE -Wall -Wextra -Wpedantic)
endif()
