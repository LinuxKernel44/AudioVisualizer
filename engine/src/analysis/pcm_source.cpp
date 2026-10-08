#include "av/analysis/pcm_source.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace av {

size_t VectorPcmSource::read(float* out, size_t frames) {
    const size_t total = data_.size() / 2;
    const size_t n = std::min(frames, total - pos_);
    std::memcpy(out, data_.data() + pos_ * 2, n * 2 * sizeof(float));
    pos_ += n;
    return n;
}

bool loadWav(const std::string& path, std::vector<float>* out, double* sampleRate,
             std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open file");
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        return fail("not a RIFF/WAVE file");

    auto u16 = [&](size_t o) { return static_cast<uint16_t>(buf[o] | (buf[o + 1] << 8)); };
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(buf[o] | (buf[o + 1] << 8) | (buf[o + 2] << 16) |
                                     (static_cast<uint32_t>(buf[o + 3]) << 24));
    };
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    size_t dataOff = 0, dataLen = 0;
    for (size_t p = 12; p + 8 <= buf.size();) {
        const uint32_t len = u32(p + 4);
        if (std::memcmp(&buf[p], "fmt ", 4) == 0) {
            fmt = u16(p + 8);
            ch = u16(p + 10);
            sr = u32(p + 12);
            bits = u16(p + 22);
            if (fmt == 0xFFFE && len >= 26) fmt = u16(p + 8 + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(&buf[p], "data", 4) == 0) {
            dataOff = p + 8;
            dataLen = std::min<size_t>(len, buf.size() - dataOff);
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!dataOff || ch == 0 || (fmt != 1 && fmt != 3)) return fail("unsupported WAV format");
    const size_t bytes = bits / 8;
    const size_t frames = dataLen / (bytes * ch);
    out->resize(frames * 2);
    auto sample = [&](size_t frame, size_t c) -> float {
        const uint8_t* q = &buf[dataOff + (frame * ch + c) * bytes];
        if (fmt == 3 && bits == 32) {
            float v;
            std::memcpy(&v, q, 4);
            return v;
        }
        switch (bits) {
            case 8:
                return (static_cast<int>(q[0]) - 128) / 128.f;
            case 16:
                return static_cast<int16_t>(q[0] | (q[1] << 8)) / 32768.f;
            case 24:
                return (static_cast<int32_t>((q[0] << 8) | (q[1] << 16) | (q[2] << 24)) >> 8) /
                       8388608.f;
            case 32:
                return static_cast<int32_t>(q[0] | (q[1] << 8) | (q[2] << 16) |
                                            (static_cast<uint32_t>(q[3]) << 24)) /
                       2147483648.f;
            default:
                return 0.f;
        }
    };
    for (size_t i = 0; i < frames; ++i) {
        const float l = sample(i, 0);
        (*out)[2 * i] = l;
        (*out)[2 * i + 1] = ch > 1 ? sample(i, 1) : l;
    }
    *sampleRate = sr;
    return true;
}

}  // namespace av
