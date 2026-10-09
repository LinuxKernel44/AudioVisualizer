#include "av/analysis/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace av {

const char* const kChannelNames[kNumChannels] = {
    "rms",        "centroid", "flatness", "onset", "onsetLow", "onsetMid", "onsetHigh",
    "percussive", "corr",     "width",    "pan",   "lufs",     "band0",    "band1",
    "band2",      "band3",    "band4",    "band5", "band6"};

const std::array<float, kNumBands + 1> kBandEdgesHz = {20.f,   60.f,   250.f,  500.f,
                                                       2000.f, 4000.f, 6000.f, 16000.f};

const char* sectionLabelName(SectionLabel l) {
    static const char* n[] = {"intro", "verse", "build",  "chorus",
                              "drop",  "break", "bridge", "outro"};
    return n[static_cast<size_t>(l)];
}

uint64_t Timeline::frameAt(double sec) const {
    if (numFrames == 0) return 0;
    const double f = std::floor(sec * frameRate + 0.5);
    return static_cast<uint64_t>(std::clamp(f, 0.0, static_cast<double>(numFrames - 1)));
}

float Timeline::valueAt(Ch c, double sec) const {
    const auto& v = ch[static_cast<size_t>(c)];
    if (v.empty()) return 0.f;
    const double pos = std::clamp(sec * frameRate, 0.0, static_cast<double>(v.size() - 1));
    const size_t i = static_cast<size_t>(pos);
    const size_t j = std::min(i + 1, v.size() - 1);
    const float fr = static_cast<float>(pos - static_cast<double>(i));
    return v[i] * (1.f - fr) + v[j] * fr;
}

Timeline::BeatPos Timeline::beatPosAt(double sec) const {
    BeatPos p;
    if (beats.empty()) return p;
    auto it = std::upper_bound(beats.begin(), beats.end(), static_cast<float>(sec));
    if (it == beats.begin()) return p;
    const size_t i = static_cast<size_t>(it - beats.begin()) - 1;
    p.beatIndex = static_cast<int>(i);
    p.sinceBeatSec = static_cast<float>(sec) - beats[i];
    float len;
    if (i + 1 < beats.size())
        len = beats[i + 1] - beats[i];
    else if (i > 0)
        len = beats[i] - beats[i - 1];
    else
        len = bpm > 0 ? 60.f / bpm : 0.5f;
    p.phase = std::clamp(p.sinceBeatSec / std::max(len, 1e-3f), 0.f, 1.f);
    p.barBeat = i < beatBar.size() ? beatBar[i] : 0;
    return p;
}

const Section* Timeline::sectionAt(double sec) const {
    for (const auto& s : sections)
        if (sec >= s.startSec && sec < s.endSec) return &s;
    return sections.empty() ? nullptr : &sections.back();
}

bool Timeline::operator==(const Timeline& o) const {
    return sampleRate == o.sampleRate && hop == o.hop && numFrames == o.numFrames && ch == o.ch &&
           bpm == o.bpm && beats == o.beats && beatBar == o.beatBar && key == o.key &&
           minor == o.minor && sections.size() == o.sections.size() &&
           integratedLufs == o.integratedLufs && energyCurve == o.energyCurve &&
           tempoCurve == o.tempoCurve && onsets.size() == o.onsets.size();
}

// ----------------------------------------------------------------------------------------------
namespace {

constexpr char kMagic[4] = {'A', 'V', 'T', '1'};

struct Writer {
    std::vector<uint8_t> b;
    template <typename T>
    void put(const T& v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    template <typename T>
    void putVec(const std::vector<T>& v) {
        put<uint64_t>(v.size());
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        b.insert(b.end(), p, p + v.size() * sizeof(T));
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    template <typename T>
    T get() {
        T v{};
        if (pos + sizeof(T) > n) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    template <typename T>
    std::vector<T> getVec() {
        const uint64_t cnt = get<uint64_t>();
        std::vector<T> v;
        if (!ok || cnt > (n - pos) / sizeof(T)) {
            ok = false;
            return v;
        }
        v.resize(cnt);
        std::memcpy(v.data(), p + pos, cnt * sizeof(T));
        pos += cnt * sizeof(T);
        return v;
    }
};

}  // namespace

std::vector<uint8_t> serialize(const Timeline& t) {
    Writer w;
    w.b.insert(w.b.end(), kMagic, kMagic + 4);
    w.put<uint32_t>(Timeline::kVersion);
    w.put(t.sampleRate);
    w.put(t.hop);
    w.put(t.frameRate);
    w.put<uint64_t>(t.numFrames);
    w.put(t.durationSec);
    for (const auto& c : t.ch) {
        float lo = 0.f, hi = 0.f;
        if (!c.empty()) {
            lo = *std::min_element(c.begin(), c.end());
            hi = *std::max_element(c.begin(), c.end());
        }
        w.put(lo);
        w.put(hi);
        const float scale = hi > lo ? 65535.f / (hi - lo) : 0.f;
        std::vector<uint16_t> q(c.size());
        for (size_t i = 0; i < c.size(); ++i)
            q[i] =
                static_cast<uint16_t>(std::lround(std::clamp((c[i] - lo) * scale, 0.f, 65535.f)));
        w.putVec(q);
    }
    w.put(t.bpm);
    w.put(t.bpmConfidence);
    w.putVec(t.tempoCurve);
    w.put(t.tempoCurveStepSec);
    w.putVec(t.beats);
    w.putVec(t.beatBar);
    w.put(t.beatsPerBar);
    w.put(t.downbeatConfidence);
    w.put(t.beatSource);
    w.putVec(t.onsets);
    for (float v : t.chromaMean) w.put(v);
    w.put<int32_t>(t.key);
    w.put<uint8_t>(t.minor);
    w.put(t.keyConfidence);
    w.put(t.integratedLufs);
    w.put(t.loudnessRange);
    w.put(t.peakDb);
    w.put(t.crestDb);
    w.putVec(t.sections);
    w.putVec(t.energyCurve);
    return w.b;
}

bool deserialize(const uint8_t* data, size_t size, Timeline* out, std::string* error) {
    Reader r{data, size};
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    if (size < 8 || std::memcmp(data, kMagic, 4) != 0) return fail("bad magic");
    r.pos = 4;
    if (r.get<uint32_t>() != Timeline::kVersion) return fail("unsupported version");
    Timeline t;
    t.sampleRate = r.get<double>();
    t.hop = r.get<uint32_t>();
    t.frameRate = r.get<double>();
    t.numFrames = r.get<uint64_t>();
    t.durationSec = r.get<double>();
    for (auto& c : t.ch) {
        const float lo = r.get<float>();
        const float hi = r.get<float>();
        const auto q = r.getVec<uint16_t>();
        if (!r.ok) return fail("truncated");
        c.resize(q.size());
        const float step = (hi - lo) / 65535.f;
        for (size_t i = 0; i < q.size(); ++i) c[i] = lo + static_cast<float>(q[i]) * step;
    }
    t.bpm = r.get<float>();
    t.bpmConfidence = r.get<float>();
    t.tempoCurve = r.getVec<float>();
    t.tempoCurveStepSec = r.get<float>();
    t.beats = r.getVec<float>();
    t.beatBar = r.getVec<uint8_t>();
    t.beatsPerBar = r.get<uint32_t>();
    t.downbeatConfidence = r.get<float>();
    t.beatSource = r.get<uint32_t>();
    t.onsets = r.getVec<Onset>();
    for (float& v : t.chromaMean) v = r.get<float>();
    t.key = r.get<int32_t>();
    t.minor = r.get<uint8_t>() != 0;
    t.keyConfidence = r.get<float>();
    t.integratedLufs = r.get<float>();
    t.loudnessRange = r.get<float>();
    t.peakDb = r.get<float>();
    t.crestDb = r.get<float>();
    t.sections = r.getVec<Section>();
    t.energyCurve = r.getVec<float>();
    if (!r.ok) return fail("truncated");
    // The file is only ever written by serialize(), but a corrupt or foreign cache entry must not
    // turn into out-of-range reads later.
    if (!(t.frameRate > 0) || !std::isfinite(t.frameRate) || !std::isfinite(t.durationSec))
        return fail("bad header");
    for (const auto& c : t.ch)
        if (c.size() != t.numFrames) return fail("channel length mismatch");
    if (t.beatBar.size() != t.beats.size() || !std::is_sorted(t.beats.begin(), t.beats.end()))
        return fail("bad beat grid");
    for (const Section& s : t.sections)
        if (static_cast<size_t>(s.label) > static_cast<size_t>(SectionLabel::Outro))
            return fail("bad section label");
    if (t.beatsPerBar == 0) return fail("bad meter");
    *out = std::move(t);
    return true;
}

}  // namespace av
