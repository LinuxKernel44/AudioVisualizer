# AudioVisualizer

Turn any audio file into a music-reactive video. Import a track, let the engine analyse it, watch a
live Vulkan visualizer that follows the song's structure, and export an H.264/AAC MP4.

The Android UI (Kotlin, Jetpack Compose) is thin. Decoding, DSP, analysis, simulation, rendering and
video export live in a multi-threaded C++20 engine reached through a single JNI facade.

> **All rights reserved.** This is a private, proprietary project.

## What it does
* **Import** MP3, AAC/M4A, FLAC, WAV, OGG/Opus and anything else Android's `MediaCodec` can decode
  (mono to 5.1, 8 kHz to 96 kHz, 16/24-bit). Streaming decode: memory does not grow with track length.
* **Analysis** (multi-threaded, cached per file): tempo and tempo curve, beat/downbeat grid and meter,
  onsets (kick/snare/hat bands), key, BS.1770 loudness (LUFS/LRA), peak/crest, stereo width/pan/correlation,
  harmonic/percussive share, seven band energies, chroma and a segmentation into sections
  (intro/verse/build/chorus/drop/break/bridge/outro). An optional ONNX model
  ([Beat This!](docs/ML.md)) refines beats and downbeats when it is trustworthy.
* **Live visualizer** (Vulkan 1.1, HDR): spectrum bars, radial spectrum with waveform ring, kaleidoscope
  tunnel, Julia fractal, 260k-particle GPU storm. Bloom, feedback trails, chromatic aberration, glitch,
  grain and tone mapping. A *director* picks scenes, palettes and looks from the song structure and
  switches on beats; you can override it, tune sensitivity / band gains / smoothing / bloom / palette, add
  a title, artist, logo and background image.
* **Export** a deterministic MP4 (H.264 High + AAC-LC): 720p to 4K, 16:9 / 9:16 / 1:1, 24 to 120 fps,
  quality presets, in a foreground service with a progress notification, saved to `Movies/AudioVisualizer`.
* **Performance management**: GPU timestamp driven dynamic resolution, thermal throttling response, ADPF
  hints, battery saver (30 fps), performance overlay.

## Measured on a Galaxy S10 (Mali-G76, the slow baseline)
| | |
|---|---|
| Analysis of a 40 s track | 3 s (DSP) / ~10 s with the beat model |
| Analysis of a 20 min MP3 / AAC | 18 s / ~60-90 s (decode bound; compute is ~1 s on a PC), memory flat at ~170 MB |
| Preview | 60 fps for bars / radial / tunnel / fractal; particles adapt resolution to hold 60 fps |
| Export 720p30 / 1080p60 / 4K30 | 5.7x / 2.0x / 0.7x real time |

## Measured on a OnePlus 15 (Snapdragon SM8850, Android 16)
| | |
|---|---|
| Analysis of a 40 s track | 2.0 s including the beat model |
| Analysis of a 20 min MP3 / AAC / 1 h MP3 | 2.9 s / 15.5 s / 8.5 s (AAC is decoder bound) |
| Preview | every scene at the display rate with GPU time 4-12 ms; OxygenOS keeps unlisted apps at 60 Hz |
| Export 720p30 / 1080p30 / 1080p60 / 1080p120 | 13.7x / 9.0x / 4.5x / ~2.2x real time |

Measured with synthetic test material; 1440p and 4K were not measured on this device yet.

## Repository layout
```
app/        Kotlin + Compose UI, services, settings, presets, branding, JVM and instrumented tests
engine/     C++20 engine (core, audio, dsp, analysis, ml, sim, render, export, jni) + host tests/tools
shaders/    GLSL (compiled to SPIR-V by the NDK glslc at build time and embedded)
presets/    presets.json published with each release (preset updates)
tools/ml/   scripts that export the beat model to ONNX and generate numerical test references
third_party/  vendored PFFFT
docs/       ARCHITECTURE.md, ML.md
```
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design (threading, render graph, director,
export pipeline) and [docs/ML.md](docs/ML.md) for the beat model.

## Requirements
Android 12+ (API 31), arm64-v8a, Vulkan 1.1. Build: JDK 17+, Android SDK 36, NDK r28, CMake 3.31.

## Build and test
```sh
./gradlew :app:assembleDebug                       # debug APK
./gradlew :app:testDebugUnitTest                   # Kotlin JVM tests (presets JSON)
./gradlew :app:connectedDebugAndroidTest           # 15 instrumented tests, needs a device
cmake -S engine -B build/host -G Ninja -DAV_BUILD_TESTS=ON -DAV_BUILD_TOOLS=ON
cmake --build build/host && ctest --test-dir build/host   # 74 native tests on the host
build/host/av_inspect cache.avt                    # dump an analysis cache as JSON
```
`./gradlew :app:assembleRelease` produces an R8-minified APK; it is signed when `keystore.properties`
(`storeFile`, `storePassword`, `keyAlias`, `keyPassword`) exists. Tagging `vX.Y.Z` runs the release
workflow, which needs the repository secrets `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`.

## Privacy and permissions
No analytics, no ads, no background network use. `INTERNET` is used only when you press *Check for
updates* in Settings to fetch `presets.json` from the project's GitHub Releases with a token you provide
(stored encrypted with an Android Keystore key). Audio is read through the system file picker; exports are
written to `Movies/AudioVisualizer`.

## Known limitations
* Tempo octave errors are possible with the DSP tracker (e.g. 174 BPM can resolve to 87).
* Section labels are heuristic. Time signatures other than 3/4 and 4/4 are not detected.
* The beat model has not been evaluated on real music in this repository (only synthetic material was
  available); it is gated and falls back to the DSP grid when its output looks wrong.
* Only H.264 export; the export renders on the GPU and copies frames once (no zero-copy encoder path).
