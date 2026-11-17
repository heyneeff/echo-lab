// render_verb: fire an impulse (or a WAV) through the Lethe reverb engine and write the wet result.
//   render_verb out.wav [--in in.wav] [--style 0] [--decay 2.5] [--size 1] [--damp 6000] [--bass 1]
//               [--diff 1] [--mod 1] [--early 1] [--pre 10] [--freeze 0] [--secs 6] [--mix 1]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../dsp/ReverbEngine.h"

using namespace echolab::verb;

static bool readWav(const char* path, std::vector<float>& L, std::vector<float>& R, int& sr) {  // float32 or 16-bit
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    char id[4]; uint32_t sz; std::fread(id, 1, 4, f); std::fread(&sz, 4, 1, f); std::fread(id, 1, 4, f);
    int fmt = 1, bits = 16, ch = 2;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&sz, 4, 1, f) == 1) {
        if (!std::strncmp(id, "fmt ", 4)) { uint16_t af, nc, ba, bp; uint32_t rate, br; std::fread(&af, 2, 1, f); std::fread(&nc, 2, 1, f);
            std::fread(&rate, 4, 1, f); std::fread(&br, 4, 1, f); std::fread(&ba, 2, 1, f); std::fread(&bp, 2, 1, f);
            fmt = af; ch = nc; sr = (int)rate; bits = bp; if (sz > 16) std::fseek(f, sz - 16, SEEK_CUR); }
        else if (!std::strncmp(id, "data", 4)) { size_t n = sz / (bits / 8) / ch; L.resize(n); R.resize(n);
            for (size_t i = 0; i < n; ++i) for (int c = 0; c < ch; ++c) { float v;
                if (fmt == 3) std::fread(&v, 4, 1, f); else { int16_t t; std::fread(&t, 2, 1, f); v = t / 32768.f; }
                if (c == 0) L[i] = v; if (c == 1 || ch == 1) R[i] = v; }
            break; }
        else std::fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    std::fclose(f); return !L.empty();
}

static void writeWav(const char* path, const std::vector<float>& inter, int sr) {
    FILE* f = std::fopen(path, "wb");
    uint32_t n = (uint32_t)inter.size(), dataBytes = n * 4, riff = 36 + dataBytes, rate = sr, br = sr * 8;
    uint16_t fmt = 3, ch = 2, ba = 8, bits = 32; uint32_t fsz = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fsz, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f);
    std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(inter.data(), 4, n, f); std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: render_verb out.wav [options]\n"); return 1; }
    const char* out = argv[1];
    Params p; p.mix = 1.0; int style = 0; double secs = 6, freezeAt = -1; int sr = 48000; const char* inPath = nullptr;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i]; double v = std::atof(argv[i + 1]);
        if (k == "--style") style = (int)v; else if (k == "--decay") p.decayS = v; else if (k == "--size") p.size = v;
        else if (k == "--damp") p.dampingHz = v; else if (k == "--bass") p.bass = v; else if (k == "--diff") p.diffusion = v;
        else if (k == "--mod") p.modulation = v; else if (k == "--early") p.early = v; else if (k == "--pre") p.predelayMs = v;
        else if (k == "--freeze") p.freeze = v > 0.5; else if (k == "--secs") secs = v; else if (k == "--mix") p.mix = v; else if (k == "--freezeAt") freezeAt = v;
        else if (k == "--in") inPath = argv[i + 1]; else if (k == "--lowcut") p.lowCutHz = v; else if (k == "--duck") p.duck = v;
        else if (k == "--release") p.duckReleaseMs = v; else if (k == "--width") p.width = v;
    }
    std::vector<float> L, R;
    if (inPath) { if (!readWav(inPath, L, R, sr)) { std::fprintf(stderr, "cannot read %s\n", inPath); return 1; } L.resize(L.size() + (size_t)(secs * sr), 0.f); R.resize(L.size(), 0.f); }
    else { L.assign((size_t)(secs * sr), 0.f); R.assign(L.size(), 0.f); L[0] = R[0] = 1.0f; }  // impulse
    ReverbEngine e; e.prepare(sr); e.update(p, kVerbStyles[std::clamp(style, 0, 7)]);
    const int n = (int)L.size(), block = 256;
    // optional noise burst source (so freeze has something to hold)
    if (freezeAt > 0) for (int i = 0; i < (int)(0.4 * sr); ++i) { double r = (std::rand() / (double)RAND_MAX * 2 - 1) * 0.3; L[i] = R[i] = (float)r; }
    for (int i = 0; i < n; i += block) {
        if (freezeAt > 0 && i >= (int)(freezeAt * sr) && !e.params.freeze) { Params q = e.params; q.freeze = true; e.update(q, e.style); }
        e.process(&L[i], &R[i], std::min(block, n - i));
    }
    std::vector<float> inter(2 * n);
    double peak = 0; bool bad = false;
    for (int i = 0; i < n; ++i) { inter[2 * i] = L[i]; inter[2 * i + 1] = R[i]; peak = std::max(peak, (double)std::abs(L[i])); if (!std::isfinite(L[i])) bad = true; }
    writeWav(out, inter, sr);
    std::printf("{\"peak\": %.5f, \"finite\": %s}\n", peak, bad ? "false" : "true");
    return bad ? 2 : 0;
}
