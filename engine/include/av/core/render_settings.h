#pragma once

namespace av {

/// User-adjustable look-and-feel controls, shared by the live preview and the exporter.
struct RenderSettings {
    float sensitivity = 1.f;  // multiplier on the normalised spectrum (0.25 .. 4)
    float gainBass = 0.f;     // dB offsets applied to the low / mid / high thirds of the spectrum
    float gainMid = 0.f;
    float gainTreble = 0.f;
    float smoothing = 0.3f;   // 0 = snappy, 1 = floaty (sets the release time constant)
    int palette = -1;         // -1 = follow the structure, else a palette index
    float beatFlash = 1.f;    // scales beat/kick flashes (0 disables)
    float bloom = 1.f;        // multiplier on the scene's bloom
};

}  // namespace av
