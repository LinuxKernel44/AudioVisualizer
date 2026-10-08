cmake_minimum_required(VERSION 3.22)
project(AudioVisualizerEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AV_BUILD_TESTS "Build host unit tests" OFF)

add_library(avcore STATIC src/core/version.cpp)
target_include_directories(avcore PUBLIC include)
target_compile_options(avcore PRIVATE -Wall -Wextra -Wpedantic)

if(ANDROID)
    add_library(avengine SHARED src/jni/engine_jni.cpp)
    target_link_libraries(avengine PRIVATE avcore log)
endif()

if(AV_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
    add_executable(av_tests tests/version_test.cpp)
    target_link_libraries(av_tests PRIVATE avcore GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(av_tests)
endif()
