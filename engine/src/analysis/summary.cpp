#include "av/analysis/summary.h"

#include <cmath>
#include <cstdio>

namespace av {

const char* pitchClassName(int pc) {
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return pc >= 0 && pc < 12 ? n[pc] : "?";
}

namespace {
void num(std::string& s, double v, int decimals = 3) {
    char buf[48];
    if (!std::isfinite(v)) v = 0;
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    s += buf;
}
}  // namespace

std::string timelineSummaryJson(const Timeline& t) {
    std::string s = "{";
    s += "\"durationSec\":";
    num(s, t.durationSec, 2);
    s += ",\"bpm\":";
    num(s, t.bpm, 1);
    s += ",\"bpmConfidence\":";
    num(s, t.bpmConfidence, 2);
    s += ",\"beatsPerBar\":" + std::to_string(t.beatsPerBar);
    s += ",\"beatSource\":\"";
    s += t.beatSource == 1 ? "ml" : "dsp";
    s += "\"";
    s += ",\"key\":\"";
    s += pitchClassName(t.key);
    s += "\",\"minor\":";
    s += t.minor ? "true" : "false";
    s += ",\"keyConfidence\":";
    num(s, t.keyConfidence, 2);
    s += ",\"lufs\":";
    num(s, t.integratedLufs, 1);
    s += ",\"lra\":";
    num(s, t.loudnessRange, 1);
    s += ",\"peakDb\":";
    num(s, t.peakDb, 1);
    s += ",\"crestDb\":";
    num(s, t.crestDb, 1);
    s += ",\"sections\":[";
    for (size_t i = 0; i < t.sections.size(); ++i) {
        const Section& c = t.sections[i];
        if (i) s += ",";
        s += "{\"start\":";
        num(s, c.startSec, 2);
        s += ",\"end\":";
        num(s, c.endSec, 2);
        s += ",\"label\":\"";
        s += sectionLabelName(c.label);
        s += "\",\"energy\":";
        num(s, c.energy, 2);
        s += ",\"cluster\":" + std::to_string(c.cluster) + "}";
    }
    s += "],\"beats\":[";
    for (size_t i = 0; i < t.beats.size(); ++i) {
        if (i) s += ",";
        num(s, t.beats[i], 2);
    }
    s += "],\"downbeats\":[";
    bool first = true;
    for (size_t i = 0; i < t.beats.size() && i < t.beatBar.size(); ++i) {
        if (t.beatBar[i] != 0) continue;
        if (!first) s += ",";
        first = false;
        num(s, t.beats[i], 2);
    }
    s += "],\"energy\":[";
    for (size_t i = 0; i < t.energyCurve.size(); ++i) {
        if (i) s += ",";
        num(s, t.energyCurve[i], 2);
    }
    s += "]}";
    return s;
}

}  // namespace av
