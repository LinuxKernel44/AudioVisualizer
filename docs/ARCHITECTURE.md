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

## M2 status (offline analysis)
`engine/src/analysis`: `analyzeAudio()` reads PCM from a `PcmSource`, runs BS.1770 loudness
sequentially and slices the stream into self-contained chunks (500 frames + 9 frames of context)
processed by a `ThreadPool`. Results are independent of the thread count (tested).
Per-frame channels (see `Ch`): rms, centroid, flatness, onset strength (full and low/mid/high),
HPSS percussive share, stereo correlation/width/pan, momentary LUFS, 7 band energies.
Global: tempo (+ curve), DP beat tracking, meter (3/4 vs 4/4) and bar phase, onsets, key,
integrated LUFS / LRA / peak / crest, sections (Foote novelty + clustering + heuristic labels)
and a per-second energy curve. Serialised as `AVT1` (16-bit quantised channels).

Known limits: tempo octave errors are possible (e.g. 174 BPM resolves to 87); section labels are
heuristic until the ML stage (M3); time signatures other than 3/4 and 4/4 are not detected.

## M4/M5 status (rendering, performance, export)
**Render pipeline** (`engine/src/render`): `VkContext` -> `RenderCore` (per-frame uniforms, scenes,
`PostChain`) shared by the on-screen `VulkanRenderer` and the offline `OffscreenRenderer`. Scenes
render linear HDR (RGBA16F, falling back to packed float / RGBA8) into a target; `PostChain` adds
optional scene-transition mix, feedback trails, dual-filter bloom and a composite pass (ACES tone
map, chromatic aberration, glitch, grain, vignette, sRGB encode). Shaders are GLSL compiled with the
NDK `glslc` and embedded as SPIR-V headers.

**Scenes**: spectrum bars, radial spectrum + waveform ring, kaleidoscope tunnel, Julia orbit-trap
fractal, GPU particle storm (compute, up to 262k particles, count follows the quality level).

**Director** (`sim/director`): the scene/look at a given time is a pure function of the playback
position and the analysis `Timeline` (sections -> scene + palette + look, transitions one bar of two
beats long), so seeking and export are exact. Manual selection overrides it with a short blend.

**FrontEnd** (`sim/front_end`): audio -> smoothed bars/waveform/beat fields -> `VisualState`.
Deterministic and shared by the live worker and the exporter. `RenderSettings` (sensitivity, band
gains, smoothing, palette, beat flash, bloom) are live controls.

**Performance**: GPU timestamp queries feed a dynamic resolution controller (scale 0.5..1, hysteresis),
the Android thermal status caps the scale, ADPF hint sessions (API 33+, weak-linked) describe the frame
budget, and an on-screen overlay shows fps / GPU ms / scale / thermal level.

**Export** (`engine/src/export`): decode (MediaCodec) -> FrontEnd -> `OffscreenRenderer` (fixed time
step) -> GPU RGBA->NV12 compute -> two-deep async readback -> H.264 (MediaCodec, BT.709 limited) and
AAC-LC -> `MuxSink` (MP4). The audio is decoded once and feeds both visuals and AAC. A foreground
service runs the job and writes into MediaStore (Movies/AudioVisualizer). A zero-copy path
(AImageWriter + AHardwareBuffer) was evaluated and not adopted: vendor-dependent encoder input formats
and weaker PTS control; the current path keeps timestamps explicit and output reproducible.

Measured on a Galaxy S10 (Mali-G76): 40 s track -> 720p30 in 7 s (5.7x real time), 1080p60 in 20 s
(2.0x), 4K30 in 59 s (0.7x). The OnePlus 15 has not been measured yet.
