# fifastreet3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fifastreet3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FIFASTREET3_SOURCES
    src/main.cpp
    src/pc_options.cpp
    src/native/gfx_system.cpp
    src/native/renderer.cpp
    src/native/guest_texture.cpp
    src/native/native_hooks.cpp
    src/native/xenos_hlsl.cpp
    src/native/xma_engine.cpp
    src/native/audio_system.cpp
    src/native/audio_exports.cpp
)

if(WIN32)
    add_executable(fifastreet3 WIN32 ${FIFASTREET3_SOURCES})
else()
    add_executable(fifastreet3 ${FIFASTREET3_SOURCES})
endif()

rexglue_setup_target(fifastreet3)
target_link_libraries(fifastreet3 PRIVATE d3d11 d3d12 dxgi rex::libavcodec rex::libavutil)
# Public headers of the SDK's FFmpeg build (LGPL 2.1, see third_party/ffmpeg) for the XMA decoder.
target_include_directories(fifastreet3 PRIVATE third_party/ffmpeg/include)

# Offline check of the Xenos -> HLSL translator (compiles the extracted shaders with FXC).
add_executable(fs3_shader_test tools/shader_test.cpp src/native/xenos_hlsl.cpp)
target_link_libraries(fs3_shader_test PRIVATE rex::runtime)
