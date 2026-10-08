#include "av/dsp/bar_spectrum.h"

#include <algorithm>
#include <cmath>

namespace av {

namespace {
size_t lowBarCount(size_t numBars, double minHz, double maxHz, double crossoverHz) {
    const double frac = std::log(crossoverHz / minHz) / std::log(maxHz / minHz);
    return std::clamp<size_t>(static_cast<size_t>(std::lround(frac * static_cast<double>(numBars))),
                              1, numBars - 1);
}
}  // namespace

BarSpectrum::BarSpectrum(double sampleRate, size_t numBars, double minHz, double maxHz,
                         double crossoverHz)
    : numLow_(lowBarCount(numBars, minHz, maxHz, crossoverHz)),
      numHigh_(numBars - numLow_),
      low_(kLowSize, sampleRate, numLow_, minHz, crossoverHz),
      high_(kHighSize, sampleRate, numHigh_, crossoverHz, maxHz) {}

void BarSpectrum::analyze(const float* recent, float* out) {
    low_.analyze(recent, out);
    high_.analyze(recent + (kLowSize - kHighSize), out + numLow_);
}

}  // namespace av
