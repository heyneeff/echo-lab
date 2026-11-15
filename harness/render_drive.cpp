// render_drive: run DriveEngine offline over a WAV.
//   render_drive in.wav out.wav --voice-json v.json --drive 5 [--punish 1] [--mix 1] [--auto 0]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../dsp/DriveEngine.h"

namespace {

struct Wav { int sr = 44100, ch = 2; std::vector<float> data; };  // interleaved

bool readWav(const char* path, Wav& w) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char id[4]; uint32_t sz;
    std::fread(id, 1, 4, f); std::fread(&sz, 4, 1, f); std::fread(id, 1, 4, f);
    int fmt = 1, bits = 16;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&sz, 4, 1, f) == 1) {
        if (!std::strncmp(id, "fmt ", 4)) {
            uint16_t af, nc, ba, bp; uint32_t rate, br;
            std::fread(&af, 2, 1, f); std::fread(&nc, 2, 1, f); std::fread(&rate, 4, 1, f);
            std::fread(&br, 4, 1, f); std::fread(&ba, 2, 1, f); std::fread(&bp, 2, 1, f);
            fmt = af; w.ch = nc; w.sr = (int)rate; bits = bp;
            if (sz > 16) std::fseek(f, sz - 16, SEEK_CUR);
        } else if (!std::strncmp(id, "data", 4)) {
            size_t n = sz / (bits / 8);
            w.data.resize(n);
            if (fmt == 3 && bits == 32) std::fread(w.data.data(), 4, n, f);
            else if (bits == 16) { std::vector<int16_t> t(n); std::fread(t.data(), 2, n, f); for (size_t i = 0; i < n; ++i) w.data[i] = t[i] / 32768.f; }
            else if (bits == 24) { for (size_t i = 0; i < n; ++i) { uint8_t b[3]; std::fread(b, 1, 3, f); int32_t v = (b[0] << 8 | b[1] << 16 | b[2] << 24) >> 8; w.data[i] = v / 8388608.f; } }
            else { std::fclose(f); return false; }
            break;
        } else std::fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    std::fclose(f);
    return !w.data.empty();
}

bool writeWav(const char* path, const Wav& w) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t dataBytes = (uint32_t)(w.data.size() * 4), riff = 36 + dataBytes, fmtSz = 16, rate = w.sr, br = w.sr * w.ch * 4;
    uint16_t af = 3, nc = (uint16_t)w.ch, ba = (uint16_t)(w.ch * 4), bp = 32;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSz, 4, 1, f); std::fwrite(&af, 2, 1, f); std::fwrite(&nc, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bp, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(w.data.data(), 4, w.data.size(), f);
    std::fclose(f);
    return true;
}


bool loadVoice(const char* path, echolab::DriveVoice& v) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string js; char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n);
    std::fclose(f);
    auto num = [&](const char* key, double& out) {
        std::string k = std::string("\"") + key + "\"";
        size_t p = js.find(k);
        if (p == std::string::npos) return;
        p = js.find(':', p);
        if (p != std::string::npos) out = std::atof(js.c_str() + p + 1);
    };
    double inv = v.invert ? 1 : 0;
    num("gainPerDriveDb", v.gainPerDriveDb); num("baseGainDb", v.baseGainDb); num("ceiling", v.ceiling);
    num("knee", v.knee); num("asym", v.asym); num("bias", v.bias); num("biasTrack", v.biasTrack); num("emphHz", v.emphHz);
    num("emphDb", v.emphDb); num("lowHz", v.lowHz); num("highHz", v.highHz); num("tiltDb", v.tiltDb);
    num("invert", inv); num("outDb", v.outDb); num("punishDb", v.punishDb);
    v.invert = inv > 0.5;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: render_drive in.wav out.wav --voice-json v.json [--drive 5]\n"); return 1; }
    Wav in;
    if (!readWav(argv[1], in)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    echolab::DriveVoice voice;
    echolab::DriveParams p;
    p.autoGain = false;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::string k = argv[i]; double v = std::atof(argv[i + 1]);
        if (k == "--voice-json") { if (!loadVoice(argv[i + 1], voice)) { std::fprintf(stderr, "bad voice\n"); return 1; } }
        else if (k == "--drive") p.drive = v;
        else if (k == "--punish") p.punish = v > 0.5;
        else if (k == "--mix") p.mix = v;
        else if (k == "--auto") p.autoGain = v > 0.5;
    }
    echolab::DriveEngine eng;
    eng.prepare(in.sr);
    eng.setVoice(voice);
    eng.update(p);
    eng.reset();
    size_t frames = in.data.size() / in.ch;
    std::vector<float> L(frames), R(frames);
    for (size_t i = 0; i < frames; ++i) { L[i] = in.data[i * in.ch]; R[i] = in.ch > 1 ? in.data[i * in.ch + 1] : L[i]; }
    for (size_t i = 0; i < frames; i += 256) eng.process(L.data() + i, R.data() + i, (int)std::min<size_t>(256, frames - i));
    Wav out; out.sr = in.sr; out.ch = 2; out.data.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) { out.data[2 * i] = L[i]; out.data[2 * i + 1] = R[i]; }
    return writeWav(argv[2], out) ? 0 : 1;
}
