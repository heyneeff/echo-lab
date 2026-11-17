// Wavetable files: the common single-cycle-sequence .wav format (N frames of 2048 samples each,
// usually with a "clm " chunk "<!>2048 ..."), used by most wavetable synths.
//   import: each cycle -> its exact spectrum (no pitch detection), up to 64 frames picked evenly
//   export: 64 frames x 2048 float samples, band-limited (from level 1: <= 512 harmonics)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WavetableSynth.h"

namespace echolab {
namespace wt {

inline std::vector<Spectrum> cyclesToSpectra(const float* x, int n, int cycle, int maxFrames = 64) {
    std::vector<Spectrum> out;
    const int count = n / cycle;
    if (count < 1 || cycle < 16) return out;
    const int frames = std::min(count, maxFrames);
    std::vector<std::complex<double>> buf;
    int N = 1;
    while (N < cycle) N <<= 1;
    for (int k = 0; k < frames; ++k) {
        const int src = frames == 1 ? 0 : (int)std::lround((double)k * (count - 1) / (frames - 1));
        const float* c = x + (size_t)src * cycle;
        Spectrum s;
        s.fill(0.0);
        const int H = std::min(kMaxHarm, cycle / 2 - 1);
        if (N == cycle) {  // the usual case (2048): FFT
            buf.assign(N, 0.0);
            for (int j = 0; j < N; ++j) buf[j] = c[j];
            fft(buf);
            for (int h = 1; h <= H; ++h) s[h] = std::conj(buf[h]) * (2.0 / N);
        } else {           // odd cycle lengths: direct DFT
            for (int h = 1; h <= H; ++h) {
                std::complex<double> a = 0;
                for (int j = 0; j < cycle; ++j) a += (double)c[j] * std::polar(1.0, -2 * kPi * h * j / cycle);
                s[h] = a * (2.0 / cycle);
            }
        }
        out.push_back(s);
    }
    if (out.size() == 1) out.push_back(out[0]);  // a single cycle still needs two frames to morph between
    return out;
}

// 2048-sample cycles from a table (every second sample of level 1, which holds <= 512 harmonics)
inline std::vector<float> tableToCycles(const Wavetable& t) {
    std::vector<float> out((size_t)t.frames * 2048);
    for (int f = 0; f < t.frames; ++f) {
        const float* d = t.at(f, 1);
        for (int j = 0; j < 2048; ++j) out[(size_t)f * 2048 + j] = d[2 * j];
    }
    return out;
}

// read a wavetable .wav (PCM 16/24/32 or float 32, any channels -> mono). cycle = the clm chunk's "<!>N",
// else 2048 when the length is whole 2048 cycles. false if it isn't a usable wavetable.
inline bool readWavetableWav(const std::string& path, std::vector<float>& mono, int& cycle) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> b;
    unsigned char tmp[65536];
    size_t got;
    while ((got = std::fread(tmp, 1, sizeof tmp, f)) > 0 && b.size() < (64u << 20)) b.insert(b.end(), tmp, tmp + got);
    std::fclose(f);
    auto u32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    auto u16 = [&](size_t o) { return (uint16_t)(b[o] | b[o + 1] << 8); };
    if (b.size() < 44 || std::memcmp(&b[0], "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) return false;
    int fmt = 0, ch = 0, bits = 0;
    size_t dataAt = 0, dataLen = 0;
    cycle = 0;
    for (size_t o = 12; o + 8 <= b.size();) {
        const uint32_t len = u32(o + 4);
        if (!std::memcmp(&b[o], "fmt ", 4) && o + 24 <= b.size()) { fmt = u16(o + 8); ch = u16(o + 10); bits = u16(o + 22); if (fmt == 0xFFFE && o + 34 <= b.size()) fmt = u16(o + 32); }
        else if (!std::memcmp(&b[o], "clm ", 4)) { std::string t((const char*)&b[o + 8], std::min<size_t>(len, std::min<size_t>(64, b.size() - o - 8))); const auto k = t.find("<!>"); if (k != std::string::npos) cycle = std::atoi(t.c_str() + k + 3); }
        else if (!std::memcmp(&b[o], "data", 4)) { dataAt = o + 8; dataLen = std::min<size_t>(len, b.size() - dataAt); }
        o += 8 + (size_t)len + (len & 1);
    }
    if (!dataAt || ch < 1 || !(fmt == 1 || fmt == 3) || !(bits == 16 || bits == 24 || bits == 32)) return false;
    const int bps = bits / 8;
    const size_t frames = dataLen / ((size_t)bps * ch);
    mono.assign(frames, 0.f);
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0;
        for (int c = 0; c < ch; ++c) {
            const size_t o = dataAt + (i * ch + c) * bps;
            double v;
            if (fmt == 3 && bits == 32) { float x; std::memcpy(&x, &b[o], 4); v = x; }
            else if (bits == 16) v = (int16_t)u16(o) / 32768.0;
            else if (bits == 24) v = ((int32_t)(b[o] << 8 | b[o + 1] << 16 | b[o + 2] << 24) >> 8) / 8388608.0;
            else v = (int32_t)u32(o) / 2147483648.0;
            acc += v;
        }
        mono[i] = (float)(acc / ch);
    }
    if (cycle <= 0 && frames % 2048 == 0 && frames >= 4096) cycle = 2048;
    return cycle >= 16 && (int)frames >= cycle;
}

inline bool writeWavetableWav(const std::string& path, const std::vector<float>& cycles, int sampleRate = 48000) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::string clm = "<!>2048 10000000 wavetable (Morpheus)";
    const uint32_t clmLen = (uint32_t)clm.size() + (clm.size() & 1);
    const uint32_t dataBytes = (uint32_t)(cycles.size() * 4);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(4 + (8 + 16) + (8 + clmLen) + (8 + dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(3); u16(1); u32(sampleRate); u32(sampleRate * 4); u16(4); u16(32);  // float32 mono
    std::fwrite("clm ", 1, 4, f); u32(clmLen); std::fwrite(clm.data(), 1, clm.size(), f); if (clm.size() & 1) std::fputc(0, f);
    std::fwrite("data", 1, 4, f); u32(dataBytes); std::fwrite(cycles.data(), 4, cycles.size(), f);
    return std::fclose(f) == 0;
}

}  // namespace wt
}  // namespace echolab
