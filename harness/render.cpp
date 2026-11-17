// render: run EchoEngine offline over a WAV (for measuring against reference profiles).
//
//   render in.wav out.wav --style "Studio Tape" --time 250 --feedback 0.5 --mix 1
//          [--sat 0] [--lowcut 0] [--highcut 0] [--wobble 1] [--duck 0] [--mode 0]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../dsp/EchoEngine.h"
#include "../dsp/Styles.h"

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

// Minimal reader for a flat JSON object of numbers ({"loopHPHz": 30, ...}).
bool loadStyleJson(const char* path, echolab::Style& st) {
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
    double satType = st.satType, repitch = st.repitch ? 1 : 0, poles = st.wetLPPoles;
    num("loopHPHz", st.loopHPHz); num("loopLPHz", st.loopLPHz); num("loopLP2Hz", st.loopLP2Hz);
    num("loopTiltDb", st.loopTiltDb); num("shelfHz", st.shelfHz);
    num("wetBrickHz", st.wetBrickHz); num("loopPeakHz", st.loopPeakHz); num("loopPeakDb", st.loopPeakDb); num("loopPeakQ", st.loopPeakQ); num("wetHPHz", st.wetHPHz);
    num("wetLPHz", st.wetLPHz); num("satType", satType); num("satDrive", st.satDrive);
    num("satBias", st.satBias); num("satKnee", st.satKnee); num("wetLPPoles", poles); num("wowCents", st.wowCents); num("wowRateHz", st.wowRateHz);
    num("flutterCents", st.flutterCents); num("flutterRateHz", st.flutterRateHz);
    num("noiseDb", st.noiseDb); num("noiseLPHz", st.noiseLPHz); num("repitch", repitch);
    num("timeGlideMs", st.timeGlideMs); num("outputDb", st.outputDb);
    st.satType = (int)satType; st.repitch = repitch > 0.5; st.wetLPPoles = (int)poles;
    st.name = "custom";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: render in.wav out.wav [--style name] [--time ms] ...\n"); return 1; }
    Wav in;
    if (!readWav(argv[1], in)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    echolab::Params p;
    p.mix = 1.0; p.feedback = 0.5;
    std::string style = "Studio Tape", styleJson;
    double tailSec = 2.0;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::string k = argv[i]; double v = std::atof(argv[i + 1]);
        if (k == "--style") style = argv[i + 1];
        else if (k == "--style-json") styleJson = argv[i + 1];
        else if (k == "--time") p.timeMs[0] = p.timeMs[1] = v;
        else if (k == "--timeR") p.timeMs[1] = v;
        else if (k == "--width") p.width = v;
        else if (k == "--release") p.duckReleaseMs = v;
        else if (k == "--feedback") p.feedback = v;
        else if (k == "--mix") p.mix = v;
        else if (k == "--sat") p.saturationDb = v;
        else if (k == "--lowcut") p.lowCutHz = v;
        else if (k == "--highcut") p.highCutHz = v;
        else if (k == "--wobble") p.wobble = v;
        else if (k == "--duck") p.duck = v;
        else if (k == "--mode") p.mode = (int)v;
        else if (k == "--tail") tailSec = v;
    }
    echolab::Style custom;
    const echolab::Style* s = nullptr;
    if (!styleJson.empty()) {
        if (!loadStyleJson(styleJson.c_str(), custom)) { std::fprintf(stderr, "cannot read %s\n", styleJson.c_str()); return 1; }
        s = &custom;
    } else s = echolab::findStyle(style.c_str());
    if (!s) { std::fprintf(stderr, "unknown style %s\n", style.c_str()); return 1; }
    echolab::EchoEngine eng;
    eng.prepare(in.sr);
    eng.setStyle(*s);
    eng.updateParams(p);
    eng.reset();
    size_t frames = in.data.size() / in.ch + (size_t)(tailSec * in.sr);
    std::vector<float> L(frames, 0.f), R(frames, 0.f);
    for (size_t i = 0; i < in.data.size() / in.ch; ++i) {
        L[i] = in.data[i * in.ch];
        R[i] = in.ch > 1 ? in.data[i * in.ch + 1] : L[i];
    }
    const int block = 256;
    for (size_t i = 0; i < frames; i += block) {
        int n = (int)std::min<size_t>(block, frames - i);
        eng.process(L.data() + i, R.data() + i, n);
    }
    Wav out; out.sr = in.sr; out.ch = 2; out.data.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) { out.data[2 * i] = L[i]; out.data[2 * i + 1] = R[i]; }
    if (!writeWav(argv[2], out)) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    return 0;
}
