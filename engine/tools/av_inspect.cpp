// Host tool: prints the contents of an analysis cache file (.avt) as JSON, or analyses a WAV file.
//   av_inspect file.avt            -> JSON summary of a cached analysis
//   av_inspect --wav file.wav      -> run the DSP analysis on a WAV file and print the summary
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            std::perror("open");
            return 1;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        if (!av::deserialize(bytes.data(), bytes.size(), &t, &err)) {
            std::fprintf(stderr, "avt: %s\n", err.c_str());
            return 1;
        }
    }
    std::puts(av::timelineSummaryJson(t).c_str());
    return 0;
}
