cmake_minimum_required(VERSION 3.22)
project(AudioVisualizerEngine LANGUAGES C CXX)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AV_BUILD_TESTS "Build host unit tests" OFF)
option(AV_BUILD_TOOLS "Build host command-line tools" OFF)

# --- Vendored PFFFT (C, built without warnings-as-errors) ---
set(PFFFT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/pffft)
add_library(pffft STATIC ${PFFFT_DIR}/pffft.c ${PFFFT_DIR}/pffft_common.c)
target_include_directories(pffft PUBLIC ${PFFFT_DIR})
target_compile_options(pffft PRIVATE -O3 -w)
set_target_properties(pffft PROPERTIES C_STANDARD 11)

add_library(avcore STATIC src/core/version.cpp src/dsp/spectrum.cpp src/dsp/bar_spectrum.cpp src/dsp/spectrum_worker.cpp src/dsp/stft.cpp
    src/analysis/pcm_source.cpp src/analysis/timeline.cpp src/analysis/loudness.cpp
    src/analysis/features.cpp src/analysis/rhythm.cpp src/analysis/sections.cpp
    src/ml/log_mel.cpp src/ml/resampler.cpp src/ml/beat_inference.cpp src/ml/beat_timeline.cpp
    src/analysis/analyzer.cpp src/analysis/summary.cpp src/sim/timeline_sampler.cpp src/sim/director.cpp src/sim/front_end.cpp)
target_include_directories(avcore PUBLIC include)
target_link_libraries(avcore PUBLIC pffft)
target_compile_options(avcore PRIVATE -Wall -Wextra -Wpedantic)

if(ANDROID)
    # --- Shaders: GLSL -> SPIR-V (NDK glslc) -> embedded C++ headers ---
    find_program(GLSLC glslc REQUIRED
        HINTS "${ANDROID_NDK}/shader-tools/linux-x86_64" "${ANDROID_NDK}/shader-tools/darwin-x86_64"
              "${ANDROID_NDK}/shader-tools/windows-x86_64")
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
    set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GEN_DIR})
    set(SHADER_HEADERS)
    file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS ${SHADER_DIR}/*.vert ${SHADER_DIR}/*.frag ${SHADER_DIR}/*.comp)
    foreach(path ${SHADER_SOURCES})
        get_filename_component(src ${path} NAME)
        string(REPLACE "." "_" name ${src})
        add_custom_command(
            OUTPUT ${GEN_DIR}/${name}.h
            COMMAND ${GLSLC} --target-env=vulkan1.1 -O -I ${SHADER_DIR} ${path} -o ${GEN_DIR}/${name}.spv
            COMMAND ${CMAKE_COMMAND} -DINPUT=${GEN_DIR}/${name}.spv -DOUTPUT=${GEN_DIR}/${name}.h
                    -DNAME=${name} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${path} ${SHADER_DIR}/common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            COMMENT "Compiling shader ${src}")
        list(APPEND SHADER_HEADERS ${GEN_DIR}/${name}.h)
    endforeach()
    add_custom_target(avshaders DEPENDS ${SHADER_HEADERS})

    # ONNX Runtime (unpacked from its AAR by Gradle) for the optional beat model.
    if(ORT_ROOT)
        add_library(onnxruntime SHARED IMPORTED)
        set_target_properties(onnxruntime PROPERTIES
            IMPORTED_LOCATION ${ORT_ROOT}/jni/${ANDROID_ABI}/libonnxruntime.so)
        target_include_directories(onnxruntime INTERFACE ${ORT_ROOT}/headers)
    endif()

    add_library(avengine SHARED
        src/jni/engine_jni.cpp
        src/core/engine.cpp
        src/audio/decoder.cpp
        src/audio/player.cpp
        src/analysis/analysis_job.cpp
        src/ml/beat_pass.cpp
        src/render/vk_context.cpp
        src/render/scene_common.cpp
        src/render/post_chain.cpp
        src/render/render_core.cpp
        src/render/offscreen_renderer.cpp
        src/export/mux_sink.cpp
        src/export/encoders.cpp
        src/export/export_job.cpp
        src/render/scenes/registry.cpp
        src/render/scenes/particle_scene.cpp
        src/render/vulkan_renderer.cpp)
    add_dependencies(avengine avshaders)
    target_include_directories(avengine PRIVATE ${GEN_DIR})
    target_compile_options(avengine PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    # APIs newer than minSdk (ADPF, API 33) are weak symbols and called behind __builtin_available.
    target_compile_definitions(avengine PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
    target_link_libraries(avengine PRIVATE avcore log android nativewindow aaudio mediandk vulkan)
    if(ORT_ROOT)
        target_sources(avengine PRIVATE src/ml/onnx_beat_model.cpp)
        target_link_libraries(avengine PRIVATE onnxruntime)
        target_compile_definitions(avengine PRIVATE AV_HAS_ORT=1)
    endif()
    # 16 KB page-size support for Android 15+ devices.
    target_link_options(avengine PRIVATE -Wl,-z,max-page-size=16384)
endif()

if(AV_BUILD_TOOLS OR AV_BUILD_TESTS)
    add_executable(av_inspect tools/av_inspect.cpp)
    target_link_libraries(av_inspect PRIVATE avcore Threads::Threads)
endif()

if(AV_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    include(FetchContent)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
    add_executable(av_tests tests/version_test.cpp tests/ring_test.cpp tests/spectrum_test.cpp tests/bar_spectrum_test.cpp tests/loudness_test.cpp tests/analysis_test.cpp tests/timeline_sampler_test.cpp tests/director_test.cpp tests/front_end_test.cpp tests/ml_test.cpp)
    target_link_libraries(av_tests PRIVATE avcore GTest::gtest_main Threads::Threads)
    include(GoogleTest)
    gtest_discover_tests(av_tests)
endif()
