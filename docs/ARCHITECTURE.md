# Architecture

## Principles
- Thin Kotlin UI; all performance-critical work in C++20 behind a single JNI facade.
- Vulkan 1.1+ rendering with a light render graph; GLSL compiled to SPIR-V at build time.
- Deterministic offline mode (fixed time step) so exports are reproducible.
- No allocation or locks on the audio thread.

## Modules (`engine/`)
| Module | Responsibility |
|---|---|
| core | types, SPSC ring buffers, thread pool, clock, logging, ADPF |
| audio | streaming decode (MediaCodec), resampling, AAudio output |
| dsp | FFT (PFFFT), multi-resolution STFT, mel/log bands, NEON kernels |
| analysis | onsets, tempo, beats/downbeats, sections, chroma, loudness, stereo, HPSS, ONNX inference |
| timeline | binary analysis cache, section-to-scene director |
| sim | visual state, particle emitters, uniforms, smoothing |
| render | Vulkan device, swapchain, render graph, compute, post-processing |
| export | offline render -> AHardwareBuffer -> MediaCodec (H.264/AAC) -> MP4 |

## Threads
Decoder, analysis pool, audio output (real-time), timeline/director, simulation, render, encoder.
They communicate through SPSC ring buffers and a triple-buffered visual-state snapshot.

## Analysis
Offline pre-pass produces a cached timeline keyed by file hash. A lightweight real-time FFT
adds fine detail during playback. Beats/sections: classical DSP plus ONNX Runtime Mobile
for downbeats and structure.

## Network
INTERNET is used only for user-initiated preset/model updates from GitHub Releases, authenticated
with a read-only token the user enters in Settings (stored encrypted via Android Keystore).

## Milestones
M0 scaffolding · M1 vertical slice · M2 full analysis · M3 ML + director · M4 scenes/effects ·
M5 export · M6 polish and release.

## M1 status (vertical slice)
Implemented: SAF import -> `Decoder` (MediaCodec, streaming ring) -> `Player` (AAudio, lock-free
callback, mono tap) -> `SpectrumWorker` (PFFFT, 2048-pt Hann, 64 log bars, attack/release) ->
`TripleBuffer<VisualState>` -> `VulkanRenderer` (FIFO swapchain, fullscreen-triangle bar shader).
Known limitations, tracked for later milestones: no seek yet, stereo-only downmix of >2 channels,
low-frequency bars share FFT bins at 2048 points (needs a longer window for the bass), no
pre-rotation (compositor rotates), and the render thread is not yet ADPF-hinted.
