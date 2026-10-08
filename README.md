# AudioVisualizer

Production-grade Android app that turns any audio file into a music-reactive video.
The Android UI is thin (Kotlin + Jetpack Compose); decoding, DSP, analysis, simulation,
Vulkan rendering and video export live in a multi-threaded C++20 engine reached through JNI.

**All rights reserved.** This is a private, proprietary project.

## Status
Milestone M0 (scaffolding). See `docs/ARCHITECTURE.md` and the GitHub milestones.

## Requirements
- Android 12+ (API 31), arm64-v8a, Vulkan 1.1
- Android SDK 36, NDK r28, CMake 3.31, JDK 17+, `glslc`

## Build
```sh
./gradlew :app:assembleDebug          # APK
cmake -S engine -B build/host -G Ninja -DAV_BUILD_TESTS=ON
cmake --build build/host && ctest --test-dir build/host   # host unit tests
```

## Layout
`app/` Kotlin UI · `engine/` C++ engine · `shaders/` GLSL · `presets/` scene JSON ·
`tools/` host tools · `third_party/` vendored permissive libraries · `docs/`
