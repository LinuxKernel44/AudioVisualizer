cmake_minimum_required(VERSION 3.22)
project(AudioVisualizerEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AV_BUILD_TESTS "Build host unit tests" OFF)

# --- Vendored PFFFT (C, built without warnings-as-errors) ---
set(PFFFT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/pffft)
add_library(pffft STATIC ${PFFFT_DIR}/pffft.c ${PFFFT_DIR}/pffft_common.c)
target_include_directories(pffft PUBLIC ${PFFFT_DIR})
target_compile_options(pffft PRIVATE -O3 -w)
set_target_properties(pffft PROPERTIES C_STANDARD 11)

add_library(avcore STATIC src/core/version.cpp src/dsp/spectrum.cpp)
target_include_directories(avcore PUBLIC include)
target_link_libraries(avcore PUBLIC pffft)
target_compile_options(avcore PRIVATE -Wall -Wextra -Wpedantic)

if(ANDROID)
    add_library(avengine SHARED src/jni/engine_jni.cpp)
    target_link_libraries(avengine PRIVATE avcore log)
endif()

if(AV_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    include(FetchContent)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
    add_executable(av_tests tests/version_test.cpp tests/ring_test.cpp tests/spectrum_test.cpp)
    target_link_libraries(av_tests PRIVATE avcore GTest::gtest_main Threads::Threads)
    include(GoogleTest)
    gtest_discover_tests(av_tests)
endif()
