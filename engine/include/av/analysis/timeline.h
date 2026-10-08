#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace av {

/// Per-frame feature channels. All channels have exactly `numFrames` entries.
enum class Ch : uint32_t {
    Rms = 0,      // 0..1 (linear, soft-scaled)
    Centroid,     // 0..1 (log-frequency position of the spectral centroid)
    Flatness,     // 0..1
    Onset,        // onset strength, normalised so the 95th percentile is ~1
    OnsetLow,     // same, restricted to kick range
    OnsetMid,     // snare / body range
    OnsetHigh,    // hats / air
    Percussive,   // 0..1 share of percussive energy (HPSS)
    Correlation,  // -1..1 inter-channel correlation
    Width,        // 0..1 side/(mid+side)
    Pan,          // -1..1 (left..right)
    Loudness,     // momentary loudness, LUFS
    Band0,        // 7 band energies, 0..1 (dB mapped from kBandDbFloor, span kBandDbSpan)
    Band1,
    Band2,
    Band3,
    Band4,
    Band5,
    Band6,
    Count
};
constexpr size_t kNumChannels = static_cast<size_t>(Ch::Count);
constexpr size_t kNumBands = 7;
/// Band channels store clamp((dB - kBandDbFloor) / kBandDbSpan, 0, 1) of the band's power.
constexpr float kBandDbFloor = -90.f;
constexpr float kBandDbSpan = 102.f;
extern const char* const kChannelNames[kNumChannels];
/// Band edges in Hz: sub, bass, low-mid, mid, high-mid, presence, brilliance.
extern const std::array<float, kNumBands + 1> kBandEdgesHz;

enum class SectionLabel : uint8_t { Intro = 0, Verse, Build, Chorus, Drop, Break, Bridge, Outro };
const char* sectionLabelName(SectionLabel l);

struct Section {
    float startSec = 0.f;
    float endSec = 0.f;
    SectionLabel label = SectionLabel::Verse;
    float energy = 0.f;    // mean loudness, 0..1 relative to the track
    float novelty = 0.f;   // strength of the boundary that starts this section
    uint32_t cluster = 0;  // sections with the same cluster sound alike
};

struct Onset {
    float timeSec;
    float strength;
    uint8_t bands;  // bit0 low, bit1 mid, bit2 high
};

struct Timeline {
    static constexpr uint32_t kVersion = 2;

    double sampleRate = 0;
    uint32_t hop = 0;
    double frameRate = 0;  // frames per second
    uint64_t numFrames = 0;
    double durationSec = 0;

    std::array<std::vector<float>, kNumChannels> ch;

    // Rhythm
    float bpm = 0.f;
    float bpmConfidence = 0.f;
    std::vector<float> tempoCurve;  // BPM sampled every tempoCurveStepSec
    float tempoCurveStepSec = 2.f;
    std::vector<float> beats;      // seconds
    std::vector<uint8_t> beatBar;  // position inside the bar, 0 = downbeat
    uint32_t beatsPerBar = 4;
    float downbeatConfidence = 0.f;
    uint32_t beatSource = 0;  // 0 = classical DSP, 1 = Beat This! model
    std::vector<Onset> onsets;

    // Tonality / loudness / dynamics
    std::array<float, 12> chromaMean{};
    int key = -1;  // pitch class 0..11 (C=0), -1 unknown
    bool minor = false;
    float keyConfidence = 0.f;
    float integratedLufs = -70.f;
    float loudnessRange = 0.f;
    float peakDb = -120.f;
    float crestDb = 0.f;

    // Structure
    std::vector<Section> sections;
    std::vector<float> energyCurve;  // 1 value per second, 0..1

    float frameTime(uint64_t f) const {
        return static_cast<float>(static_cast<double>(f) / frameRate);
    }
    uint64_t frameAt(double sec) const;
    /// Linear interpolation of channel `c` at time `sec` (clamped).
    float valueAt(Ch c, double sec) const;
    /// Beat tracking position at `sec`.
    struct BeatPos {
        int beatIndex = -1;  // index into beats, -1 before the first beat
        float phase = 0.f;   // 0..1 inside the beat
        int barBeat = 0;     // position in bar
        float sinceBeatSec = 0.f;
    };
    BeatPos beatPosAt(double sec) const;
    const Section* sectionAt(double sec) const;

    bool operator==(const Timeline& o) const;
};

/// Compact binary format ("AVT1"): quantised 16-bit channels plus exact global data.
std::vector<uint8_t> serialize(const Timeline& t);
bool deserialize(const uint8_t* data, size_t size, Timeline* out, std::string* error);

}  // namespace av
