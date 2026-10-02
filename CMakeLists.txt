cmake_minimum_required(VERSION 3.20)
project(VisibleAudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---- 由 vcpkg 提供的库 ----
find_package(SDL3 CONFIG REQUIRED)
find_package(kissfft CONFIG REQUIRED)

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
add_executable(VisibleAudio WIN32 src/main.cpp)
target_link_libraries(VisibleAudio PRIVATE SDL3::SDL3 imgui_lib kissfft::kissfft-float)
target_compile_options(VisibleAudio PRIVATE /W4 /utf-8)

# 把 SDL3.dll 拷到 exe 旁边
add_custom_command(TARGET VisibleAudio POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:VisibleAudio>)