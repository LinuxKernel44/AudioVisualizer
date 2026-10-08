// Host tool: prints the contents of an analysis cache file (.avt) as JSON, or analyses a WAV file.
//   av_inspect file.avt            -> JSON summary of a cached analysis
//   av_inspect --wav file.wav      -> run the DSP analysis on a WAV file and print the summary
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "av/analysis/analyzer.h"
#include "av/analysis/pcm_source.h"
#include "av/analysis/summary.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file.avt | --wav file.wav\n", argv[0]);
        return 2;
    }
    av::Timeline t;
    std::string err;
    if (std::strcmp(argv[1], "--wav") == 0 && argc >= 3) {
        std::vector<float> pcm;
        double sr = 0;
        if (!av::loadWav(argv[2], &pcm, &sr, &err)) {
            std::fprintf(stderr, "wav: %s\n", err.c_str());
            return 1;
        }
        av::VectorPcmSource src(std::move(pcm), sr);
        if (!av::analyzeAudio(src, {}, &t, &err)) {
            std::fprintf(stderr, "analysis: %s\n", err.c_str());
            return 1;
        }
    } else {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) {
            std::perror("open");
            return 1;
        }
        std::vector<uint8_t> bytes;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
        if (!av::deserialize(bytes.data(), bytes.size(), &t, &err)) {
            std::fprintf(stderr, "avt: %s\n", err.c_str());
            return 1;
        }
    }
    std::puts(av::timelineSummaryJson(t).c_str());
    return 0;
}
