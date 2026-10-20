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
    src/pc_settings.cpp
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
target_link_libraries(fifastreet3 PRIVATE d3d11 dxgi rex::libavcodec rex::libavutil)
# Public headers of the SDK's FFmpeg build (LGPL 2.1, see third_party/ffmpeg) for the XMA decoder.
target_include_directories(fifastreet3 PRIVATE third_party/ffmpeg/include)

# Offline check of the Xenos -> HLSL translator (compiles the extracted shaders with FXC).
add_executable(fs3_shader_test tools/shader_test.cpp src/native/xenos_hlsl.cpp)
target_link_libraries(fs3_shader_test PRIVATE rex::runtime)

# Vulkan option of the settings menu: DXVK's d3d11.dll/dxgi.dll in vulkan\ next to the
# executable (pc_settings.cpp loads them by full path when "API grafica" is Vulkan).
set(DXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dxvk-3.1.1" CACHE PATH
    "DXVK release for Windows (x64/d3d11.dll, x64/dxgi.dll, LICENSE)")
if(WIN32 AND EXISTS "${DXVK_DIR}/x64/d3d11.dll")
    add_custom_command(TARGET fifastreet3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:fifastreet3>/vulkan"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/x64/d3d11.dll" "${DXVK_DIR}/x64/dxgi.dll"
                "$<TARGET_FILE_DIR:fifastreet3>/vulkan/")
    if(EXISTS "${DXVK_DIR}/LICENSE")
        add_custom_command(TARGET fifastreet3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/LICENSE"
                    "$<TARGET_FILE_DIR:fifastreet3>/vulkan/LICENSE.dxvk.txt")
    endif()
endif()
